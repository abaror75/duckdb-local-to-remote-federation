#include "duckdb/optimizer/remote_pushdown_optimizer.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/query_node/delete_query_node.hpp"
#include "duckdb/parser/query_node/insert_query_node.hpp"
#include "duckdb/parser/query_node/merge_query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/query_node/update_query_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/parsed_data/create_index_info.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/statement/alter_statement.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/parser/statement/delete_statement.hpp"
#include "duckdb/parser/statement/drop_statement.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"
#include "duckdb/parser/statement/insert_statement.hpp"
#include "duckdb/parser/statement/merge_into_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/statement/update_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/common/extra_type_info.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

CatalogPushdownResult::CatalogPushdownResult(CatalogReferenceType reference_type_p) : reference_type(reference_type_p) {
}

CatalogPushdownResult CatalogPushdownResult::Unknown() {
	return CatalogPushdownResult(CatalogReferenceType::UNKNOWN_CATALOG_REFERENCE);
}

CatalogPushdownResult CatalogPushdownResult::NoCatalogReference() {
	return CatalogPushdownResult(CatalogReferenceType::NO_CATALOG_REFERENCED);
}

CatalogPushdownResult CatalogPushdownResult::RemoteReference(Catalog &catalog) {
	CatalogPushdownResult result(CatalogReferenceType::SINGLE_REMOTE_CATALOG);
	result.catalog = catalog;
	return result;
}

RemotePushdownOptimizer::RemotePushdownOptimizer(Binder &binder)
    : binder(binder), owned_pushdown_state(make_uniq<RemotePushdownState>()), pushdown_state(*owned_pushdown_state) {
}

RemotePushdownOptimizer::RemotePushdownOptimizer(optional_ptr<RemotePushdownOptimizer> parent_p)
    : binder(parent_p->binder), parent(parent_p), pushdown_state(parent->pushdown_state) {
	// inherit table / column names from parent (for correlated subquery detection)
	local_table_names = parent->local_table_names;
}

void RemotePushdownOptimizer::FindRemoteCatalogsInSearchPath() {
	if (pushdown_state.search_path_initialized) {
		return;
	}
	pushdown_state.search_path_initialized = true;
	auto &client_data = ClientData::Get(binder.context);
	// iterate over all catalogs mentioned in the search path and check if they are remote
	auto search_path = client_data.catalog_search_path->Get();
	// Deduplicate by catalog name.
	identifier_set_t seen_remote_catalogs;
	for (auto &entry : search_path) {
		auto catalog_entry = Catalog::GetCatalogEntry(binder.context, entry.GetCatalog());
		if (!catalog_entry) {
			continue;
		}
		if (!catalog_entry->Supports(RemoteCapability::EXECUTE_QUERY_NODE)) {
			pushdown_state.local_catalogs_in_search_path.push_back(entry);
		} else {
			if (seen_remote_catalogs.insert(catalog_entry->GetName()).second) {
				pushdown_state.remote_catalogs_in_search_path.push_back(*catalog_entry);
			}
		}
	}
}

void RemotePushdownOptimizer::ResolveQualification(const QualifiedName &name, Identifier &catalog_name,
                                                   vector<Identifier> &schema_path) {
	// BindTableName resolves the "x.name" catalog-or-schema ambiguity the same way the binder does, and returns
	// [catalog, schema path..., name] - so a nested schema path survives instead of collapsing onto Catalog()
	if (name.Path().empty()) {
		return;
	}
	auto bound = Binder::BindTableName(binder.EntryRetriever(), name);
	catalog_name = bound.Catalog();
	bound.StripCatalog();
	auto &path = bound.Path();
	schema_path.assign(path.begin(), path.end() - 1);
}

optional_ptr<CatalogEntry> RemotePushdownOptimizer::LookupEntry(const Identifier &catalog_name,
                                                                const EntryLookupInfo &lookup,
                                                                const vector<Identifier> &schema_path) {
	vector<Identifier> qualification;
	if (!catalog_name.empty()) {
		qualification.push_back(catalog_name);
	}
	if (schema_path.empty()) {
		qualification.emplace_back(DEFAULT_SCHEMA);
	} else {
		qualification.insert(qualification.end(), schema_path.begin(), schema_path.end());
	}
	return Catalog::GetEntry(
	    binder.context, EntryLookupInfo(lookup, QualifiedName(std::move(qualification), lookup.GetEntryIdentifier())),
	    OnEntryNotFound::RETURN_NULL);
}

bool RemotePushdownOptimizer::EntryExistsInLocalCatalog(const EntryLookupInfo &lookup,
                                                        const vector<Identifier> &schema_path) {
	for (auto &local_entry : pushdown_state.local_catalogs_in_search_path) {
		// if the name specifies a schema use it, otherwise use the search path schema
		vector<Identifier> schema = schema_path.empty() ? vector<Identifier> {local_entry.GetSchema()} : schema_path;
		if (LookupEntry(local_entry.GetCatalog(), lookup, schema)) {
			return true;
		}
	}
	return false;
}

CatalogPushdownResult RemotePushdownOptimizer::ResolveRemoteCatalog(const Identifier &catalog_name,
                                                                    RemoteCapability capability) {
	auto catalog = Catalog::GetCatalogEntry(binder.context, catalog_name);
	if (!catalog || !catalog->Supports(capability)) {
		// a local catalog, or a catalog that does not exist
		return CatalogPushdownResult::Unknown();
	}
	return CatalogPushdownResult::RemoteReference(*catalog);
}

CatalogPushdownResult RemotePushdownOptimizer::Merge(CatalogPushdownResult a, CatalogPushdownResult b) {
	if (a.reference_type == CatalogReferenceType::NO_CATALOG_REFERENCED &&
	    b.reference_type == CatalogReferenceType::NO_CATALOG_REFERENCED) {
		// both sides refer to no catalog - result is no catalog reference, but with unified references
		auto result = CatalogPushdownResult::NoCatalogReference();
		result.used_expressions.insert(result.used_expressions.end(), a.used_expressions.begin(),
		                               a.used_expressions.end());
		result.used_expressions.insert(result.used_expressions.end(), b.used_expressions.begin(),
		                               b.used_expressions.end());
		result.used_table_constructs.insert(result.used_table_constructs.end(), a.used_table_constructs.begin(),
		                                    a.used_table_constructs.end());
		result.used_table_constructs.insert(result.used_table_constructs.end(), b.used_table_constructs.begin(),
		                                    b.used_table_constructs.end());
		result.used_nodes.insert(result.used_nodes.end(), a.used_nodes.begin(), a.used_nodes.end());
		result.used_nodes.insert(result.used_nodes.end(), b.used_nodes.begin(), b.used_nodes.end());
		return result;
	}
	if (a.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG &&
	    b.reference_type == CatalogReferenceType::NO_CATALOG_REFERENCED) {
		// swap "a" and "b" so the merge happens below
		return Merge(b, a);
	}
	if (a.reference_type == CatalogReferenceType::NO_CATALOG_REFERENCED &&
	    b.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		// "a" refers to no catalog, "b" refers to a single remote catalog
		// check if "b" supports all constructs referenced in "a"
		auto &remote_catalog = *b.catalog;
		for (auto &expr : a.used_expressions) {
			if (!remote_catalog.SupportsPushdown(expr.get())) {
				// pushdown not supported - result is UNKNOWN_CATALOG_REFERENCE
				return CatalogPushdownResult::Unknown();
			}
		}
		for (auto &table_construct : a.used_table_constructs) {
			if (!remote_catalog.SupportsPushdown(table_construct.get())) {
				// pushdown not supported - result is UNKNOWN_CATALOG_REFERENCE
				return CatalogPushdownResult::Unknown();
			}
		}
		for (auto &query_node : a.used_nodes) {
			if (!remote_catalog.SupportsPushdown(query_node.get())) {
				// pushdown not supported - result is UNKNOWN_CATALOG_REFERENCE
				return CatalogPushdownResult::Unknown();
			}
		}
		return b;
	}
	if (a.reference_type == CatalogReferenceType::UNKNOWN_CATALOG_REFERENCE ||
	    b.reference_type == CatalogReferenceType::UNKNOWN_CATALOG_REFERENCE) {
		return CatalogPushdownResult::Unknown();
	}
	// Both are SINGLE_REMOTE_CATALOG - only valid if they refer to the same catalog
	if (a.catalog == b.catalog) {
		return a;
	}
	return CatalogPushdownResult::Unknown();
}

void RemotePushdownOptimizer::Rewrite(unique_ptr<SQLStatement> &statement) {
	CatalogPushdownResult result;
	switch (statement->type) {
	case StatementType::SELECT_STATEMENT:
		result = Rewrite(*statement->Cast<SelectStatement>().node);
		break;
	case StatementType::INSERT_STATEMENT:
		result = Rewrite(*statement->Cast<InsertStatement>().node);
		break;
	case StatementType::DELETE_STATEMENT:
		result = Rewrite(*statement->Cast<DeleteStatement>().node);
		break;
	case StatementType::UPDATE_STATEMENT:
		result = Rewrite(*statement->Cast<UpdateStatement>().node);
		break;
	case StatementType::MERGE_INTO_STATEMENT:
		result = Rewrite(*statement->Cast<MergeIntoStatement>().node);
		break;
	case StatementType::CREATE_STATEMENT:
		result = RewriteStatement(statement->Cast<CreateStatement>());
		break;
	case StatementType::DROP_STATEMENT:
		result = RewriteStatement(statement->Cast<DropStatement>());
		break;
	case StatementType::ALTER_STATEMENT:
		result = RewriteStatement(statement->Cast<AlterStatement>());
		break;
	case StatementType::EXPLAIN_STATEMENT:
		Rewrite(statement->Cast<ExplainStatement>().stmt);
		return;
	default:
		return;
	}
	FinishPushdown(statement, result);
}

CatalogPushdownResult RemotePushdownOptimizer::Rewrite(QueryNode &node) {
	if (!cte_results.empty()) {
		throw InternalException(
		    "RemotePushdownOptimizer already has CTEs defined - this means no child was created correctly");
	}
	for (auto &cte_pair : node.cte_map.map) {
		const Identifier &cte_name = cte_pair.first;
		auto &cte_info = *cte_pair.second;
		CatalogPushdownResult cte_result;
		if (cte_info.query_node) {
			RemotePushdownOptimizer child_optimizer(this);
			cte_result = child_optimizer.Rewrite(*cte_info.query_node);
		} else {
			cte_result = CatalogPushdownResult::Unknown();
		}
		for (auto &key : cte_info.key_targets) {
			cte_result = Merge(cte_result, Rewrite(key));
		}
		cte_results[cte_name] = cte_result;
	}
	CatalogPushdownResult result;
	switch (node.type) {
	case QueryNodeType::SELECT_NODE:
		result = RewriteNode(node.Cast<SelectNode>());
		break;
	case QueryNodeType::INSERT_QUERY_NODE:
		result = RewriteNode(node.Cast<InsertQueryNode>());
		break;
	case QueryNodeType::DELETE_QUERY_NODE:
		result = RewriteNode(node.Cast<DeleteQueryNode>());
		break;
	case QueryNodeType::UPDATE_QUERY_NODE:
		result = RewriteNode(node.Cast<UpdateQueryNode>());
		break;
	case QueryNodeType::MERGE_QUERY_NODE:
		result = RewriteNode(node.Cast<MergeQueryNode>());
		break;
	case QueryNodeType::SET_OPERATION_NODE:
		result = RewriteNode(node.Cast<SetOperationNode>());
		break;
	case QueryNodeType::RECURSIVE_CTE_NODE:
		result = RewriteNode(node.Cast<RecursiveCTENode>());
		break;
	default:
		return CatalogPushdownResult::Unknown();
	}
	// Merge results of all CTEs defined in this scope
	// FIXME: this is only necessary because we push all CTEs, including unreferenced ones, to the result
	// if we pruned unreferenced CTEs we could remove this
	for (auto &cte_pair : node.cte_map.map) {
		auto it = cte_results.find(cte_pair.first);
		if (it != cte_results.end()) {
			result = Merge(result, it->second);
		}
	}
	if (result.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		if (!result.catalog->SupportsPushdown(node)) {
			// bail - referenced catalog does not support pushing down this node type
			result = CatalogPushdownResult::Unknown();
		}
	} else if (result.reference_type == CatalogReferenceType::NO_CATALOG_REFERENCED) {
		result.used_nodes.push_back(node);
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteNode(RecursiveCTENode &node) {
	RemotePushdownOptimizer left_optimizer(this);
	CatalogPushdownResult left_result = left_optimizer.Rewrite(*node.left);

	// for recursive CTEs - the right-hand side of the CTE can refer to the recursive CTE itself
	// we use whatever the CatalogPushdownResult of the LHS was to count this reference
	RemotePushdownOptimizer recursive_optimizer(this);
	recursive_optimizer.cte_results[node.ctename] = left_result;

	RemotePushdownOptimizer right_optimizer(&recursive_optimizer);
	CatalogPushdownResult right_result = right_optimizer.Rewrite(*node.right);

	auto result = Merge(left_result, right_result);
	for (auto &key : node.key_targets) {
		result = Merge(result, Rewrite(key));
	}
	for (auto &modifier : node.modifiers) {
		switch (modifier->type) {
		case ResultModifierType::ORDER_MODIFIER: {
			auto &order_mod = modifier->Cast<OrderModifier>();
			for (auto &order : order_mod.orders) {
				// ORDER BY entries cannot be constant-folded - a bare integer literal is a
				// positional reference there
				result = Merge(result, Rewrite(order.expression, ExpressionFoldingMode::FOLD_CHILDREN_ONLY));
			}
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER: {
			auto &limit_mod = modifier->Cast<LimitModifier>();
			if (limit_mod.limit) {
				result = Merge(result, Rewrite(limit_mod.limit));
			}
			if (limit_mod.offset) {
				result = Merge(result, Rewrite(limit_mod.offset));
			}
			break;
		}
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &distinct_mod = modifier->Cast<DistinctModifier>();
			for (auto &expr : distinct_mod.distinct_on_targets) {
				// DISTINCT ON entries cannot be constant-folded - a bare integer literal is a
				// positional reference there
				result = Merge(result, Rewrite(expr, ExpressionFoldingMode::FOLD_CHILDREN_ONLY));
			}
			break;
		}
		default:
			break;
		}
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteNode(SelectNode &node) {
	// Cross-catalog join sides recorded while rewriting our FROM clause are pushed at the end of
	// this function. Remember where our own entries begin so nested scopes stay independent.
	const auto pending_join_sides_base = pushdown_state.pending_join_sides.size();

	auto from_result = CatalogPushdownResult::NoCatalogReference();
	if (node.from_table) {
		from_result = Rewrite(node.from_table);
	}

	// Merge from_table result with all expressions to determine if the whole node can be pushed
	CatalogPushdownResult result = from_result;
	for (auto &expr : node.select_list) {
		result = Merge(result, Rewrite(expr));
	}
	if (node.where_clause) {
		result = Merge(result, Rewrite(node.where_clause));
	}
	for (auto &expr : node.groups.group_expressions) {
		// GROUP BY entries cannot be constant-folded - a bare integer literal is a
		// positional reference there
		result = Merge(result, Rewrite(expr, ExpressionFoldingMode::FOLD_CHILDREN_ONLY));
	}
	if (node.having) {
		result = Merge(result, Rewrite(node.having));
	}
	if (node.qualify) {
		result = Merge(result, Rewrite(node.qualify));
	}
	for (auto &modifier : node.modifiers) {
		switch (modifier->type) {
		case ResultModifierType::ORDER_MODIFIER: {
			auto &order_mod = modifier->Cast<OrderModifier>();
			for (auto &order : order_mod.orders) {
				// ORDER BY entries cannot be constant-folded - a bare integer literal is a
				// positional reference there
				result = Merge(result, Rewrite(order.expression, ExpressionFoldingMode::FOLD_CHILDREN_ONLY));
			}
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER: {
			auto &limit_mod = modifier->Cast<LimitModifier>();
			if (limit_mod.limit) {
				result = Merge(result, Rewrite(limit_mod.limit));
			}
			if (limit_mod.offset) {
				result = Merge(result, Rewrite(limit_mod.offset));
			}
			break;
		}
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &distinct_mod = modifier->Cast<DistinctModifier>();
			for (auto &expr : distinct_mod.distinct_on_targets) {
				// DISTINCT ON entries cannot be constant-folded - a bare integer literal is a
				// positional reference there
				result = Merge(result, Rewrite(expr, ExpressionFoldingMode::FOLD_CHILDREN_ONLY));
			}
			break;
		}
		default:
			break;
		}
	}

	// The whole node could not be pushed to one catalog, so push the individual single-remote
	// join sides recorded above, each carrying the WHERE conjuncts that only reference it.
	if (result.reference_type != CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		PushCrossCatalogJoinSides(node, pending_join_sides_base);
	}
	pushdown_state.pending_join_sides.resize(pending_join_sides_base);

	return result;
}

void RemotePushdownOptimizer::CollectTableAliases(const TableRef &ref, identifier_set_t &aliases) {
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE: {
		auto &base = ref.Cast<BaseTableRef>();
		aliases.insert(base.alias.empty() ? base.Table() : base.alias);
		break;
	}
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		if (join.left) {
			CollectTableAliases(*join.left, aliases);
		}
		if (join.right) {
			CollectTableAliases(*join.right, aliases);
		}
		break;
	}
	case TableReferenceType::TABLE_FUNCTION: {
		auto &func = ref.Cast<TableFunctionRef>();
		if (!func.alias.empty()) {
			aliases.insert(func.alias);
		} else if (func.function) {
			aliases.insert(func.function->Cast<FunctionExpression>().FunctionName());
		}
		break;
	}
	default:
		// Subqueries, expression lists and everything else expose exactly their alias
		if (!ref.alias.empty()) {
			aliases.insert(ref.alias);
		}
		break;
	}
}

void RemotePushdownOptimizer::CollectConjuncts(ParsedExpression &expr,
                                               vector<reference<ParsedExpression>> &conjuncts) {
	if (expr.GetExpressionClass() == ExpressionClass::CONJUNCTION) {
		auto &conj = expr.Cast<ConjunctionExpression>();
		if (conj.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			for (auto &child : conj.GetChildrenMutable()) {
				CollectConjuncts(*child, conjuncts);
			}
			return;
		}
	}
	conjuncts.push_back(expr);
}

bool RemotePushdownOptimizer::ContainsVolatileFunction(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<FunctionExpression>();
		EntryLookupInfo function_lookup(CatalogType::SCALAR_FUNCTION_ENTRY, func.GetQualifiedName());
		auto entry = Catalog::GetEntry(binder.context, function_lookup, OnEntryNotFound::RETURN_NULL);
		if (entry) {
			if (entry->type == CatalogType::MACRO_ENTRY) {
				// The expansion is not visible before binding and could itself be volatile
				return true;
			}
			if (entry->type == CatalogType::SCALAR_FUNCTION_ENTRY) {
				// The overload is chosen at bind time, so decline if ANY candidate is volatile.
				// This is the opposite polarity to the constant-folding test, which only needs one
				// non-volatile overload to exist: folding an expression that turns out volatile is
				// caught after binding, whereas a fragment has already been shipped.
				auto &scalar_entry = entry->Cast<ScalarFunctionCatalogEntry>();
				for (auto &overload : scalar_entry.functions.functions) {
					if (overload->GetStability() == FunctionStability::VOLATILE) {
						return true;
					}
				}
			}
		}
		// An unresolvable name is left alone: binding rejects the query anyway.
	}
	// Recurse explicitly rather than via ParsedExpressionIterator::VisitExpression<FunctionExpression>,
	// which stops descending as soon as a node matches the requested class. Arithmetic operators are
	// themselves FunctionExpressions in DuckDB, so `random() * 100000` matches on the `*` and the
	// visitor never reaches the random() underneath it.
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!found && ContainsVolatileFunction(child)) {
			found = true;
		}
	});
	return found;
}

bool RemotePushdownOptimizer::CanPushConjunctTo(const ParsedExpression &expr, const identifier_set_t &aliases) {
	// A subquery may correlate to the other side of the join or to a local table
	if (expr.HasSubquery()) {
		return false;
	}
	// WHERE cannot legally contain these, but a malformed tree must not be shipped
	if (expr.IsAggregate() || expr.IsWindow()) {
		return false;
	}
	// BuildPushableFilter pushes a copy and leaves the original in the master's WHERE, which is a
	// harmless re-application for a deterministic predicate. A volatile one would instead be two
	// independent evaluations: `amt > random() * 100000` becomes two different draws, and
	// nextval() would advance the sequence twice.
	if (ContainsVolatileFunction(expr)) {
		return false;
	}
	// Every column reference has to be explicitly qualified by a table on this side. An
	// unqualified reference cannot be attributed to a side here (binding has not happened yet),
	// so such conjuncts stay at the master.
	bool all_columns_local = true;
	bool saw_column = false;
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(expr, [&](const ColumnRefExpression &col_ref) {
		saw_column = true;
		auto &names = col_ref.ColumnNames();
		if (names.size() < 2) {
			all_columns_local = false;
			return;
		}
		// names is catalog.schema.table.column with optional leading parts - the qualifier
		// directly in front of the column name is what an alias matches
		if (aliases.find(names[names.size() - 2]) == aliases.end()) {
			all_columns_local = false;
		}
	});
	// A conjunct with no column reference at all is a constant predicate - leave it at the master
	return saw_column && all_columns_local;
}

unique_ptr<ParsedExpression> RemotePushdownOptimizer::BuildPushableFilter(optional_ptr<ParsedExpression> where_clause,
                                                                         const identifier_set_t &aliases) {
	if (!where_clause) {
		return nullptr;
	}
	vector<reference<ParsedExpression>> conjuncts;
	CollectConjuncts(*where_clause, conjuncts);

	unique_ptr<ParsedExpression> filter;
	for (auto &conjunct : conjuncts) {
		if (!CanPushConjunctTo(conjunct.get(), aliases)) {
			continue;
		}
		// A copy is pushed and the original stays in the master's WHERE. Re-applying the
		// predicate to the already reduced result is a no-op for inner joins, and for outer
		// joins the master's WHERE is what removes the NULL-extended rows, so it must remain.
		auto copy = conjunct.get().Copy();
		if (!filter) {
			filter = std::move(copy);
		} else {
			filter = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(filter),
			                                         std::move(copy));
		}
	}
	return filter;
}

//! Push every base table under a remote join subtree individually. A subtree that binds more
//! than one table cannot be replaced by a single remote scan: the scan carries one alias, while
//! the enclosing join still qualifies its columns with each original alias (o.x, oi.y). Pushing
//! the tables one at a time keeps every alias bindable. Each table still gets its own filter, so
//! the row reduction happens at the source either way - only the join itself moves to the master.
void RemotePushdownOptimizer::PushRemoteSubtreeTables(unique_ptr<TableRef> &ref, CatalogPushdownResult result,
                                                     SelectNode &node) {
	if (!ref) {
		return;
	}
	if (ref->type == TableReferenceType::JOIN) {
		// Prefer shipping the whole subtree so the join between its tables runs at the
		// source. That is the difference between sending both tables in full and sending
		// only the joined, filtered rows.
		if (PushRemoteSubtreeGrouped(ref, result, node)) {
			return;
		}
		auto &join = ref->Cast<JoinRef>();
		PushRemoteSubtreeTables(join.left, result, node);
		PushRemoteSubtreeTables(join.right, result, node);
		return;
	}
	identifier_set_t aliases;
	CollectTableAliases(*ref, aliases);
	auto filter = BuildPushableFilter(node.where_clause.get(), aliases);
	FinishPushdown(ref, result, std::move(filter));
}

bool RemotePushdownOptimizer::CollectGroupableBaseTables(
    TableRef &ref, vector<std::pair<Identifier, reference<BaseTableRef>>> &tables) {
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE: {
		auto &base = ref.Cast<BaseTableRef>();
		auto alias = base.alias.empty() ? base.Table() : base.alias;
		tables.emplace_back(alias, base);
		return true;
	}
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		// Only a plain inner join is safe to collapse: an outer join's NULL-extension
		// interacts with the filters that were pushed into the fragment.
		if (join.type != JoinType::INNER || join.ref_type != JoinRefType::REGULAR) {
			return false;
		}
		if (!join.left || !join.right) {
			return false;
		}
		return CollectGroupableBaseTables(*join.left, tables) && CollectGroupableBaseTables(*join.right, tables);
	}
	default:
		return false;
	}
}

void RemotePushdownOptimizer::RequalifyColumnRefsInExpression(unique_ptr<ParsedExpression> &expr,
                                                              const identifier_set_t &pushed_aliases,
                                                              const Identifier &fragment_alias) {
	if (!expr) {
		return;
	}
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &col_ref = expr->Cast<ColumnRefExpression>();
		auto &names = col_ref.ColumnNamesMutable();
		if (names.size() >= 2) {
			auto &qualifier = names[names.size() - 2];
			if (pushed_aliases.find(qualifier) != pushed_aliases.end()) {
				// "o"."order_id" becomes "__fed_jN"."o__order_id"
				auto prefixed = Identifier(qualifier.GetIdentifierName() + "__" +
				                           names.back().GetIdentifierName());
				names.clear();
				names.push_back(fragment_alias);
				names.push_back(prefixed);
			}
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) {
		    RequalifyColumnRefsInExpression(child, pushed_aliases, fragment_alias);
	    });
}

void RemotePushdownOptimizer::RequalifyColumnRefsInTableRef(TableRef &ref, const identifier_set_t &pushed_aliases,
                                                            const Identifier &fragment_alias) {
	// The conditions of the *enclosing* joins still reference the pushed aliases and are not
	// reachable through the query node's expression list, so walk the join tree directly.
	// Deliberately an explicit walk rather than EnumerateTableRefChildren: that helper already
	// descends into nested refs itself, so recursing from its callback re-walks each subtree
	// and never terminates.
	//
	// Subqueries are not entered - their FROM clause opens a new alias scope, so an "o" inside
	// a subquery is a different table than the "o" that was just pushed.
	if (ref.type != TableReferenceType::JOIN) {
		return;
	}
	auto &join = ref.Cast<JoinRef>();
	RequalifyColumnRefsInExpression(join.condition, pushed_aliases, fragment_alias);
	if (join.left) {
		RequalifyColumnRefsInTableRef(*join.left, pushed_aliases, fragment_alias);
	}
	if (join.right) {
		RequalifyColumnRefsInTableRef(*join.right, pushed_aliases, fragment_alias);
	}
}

void RemotePushdownOptimizer::RequalifyColumnRefs(SelectNode &node, const identifier_set_t &pushed_aliases,
                                                 const Identifier &fragment_alias) {
	for (auto &expr : node.select_list) {
		RequalifyColumnRefsInExpression(expr, pushed_aliases, fragment_alias);
	}
	RequalifyColumnRefsInExpression(node.where_clause, pushed_aliases, fragment_alias);
	for (auto &expr : node.groups.group_expressions) {
		RequalifyColumnRefsInExpression(expr, pushed_aliases, fragment_alias);
	}
	RequalifyColumnRefsInExpression(node.having, pushed_aliases, fragment_alias);
	RequalifyColumnRefsInExpression(node.qualify, pushed_aliases, fragment_alias);
	// ORDER BY / LIMIT / DISTINCT ON expressions - EnumerateQueryNodeModifiers already visits
	// every modifier, so it is called once rather than per modifier.
	ParsedExpressionIterator::EnumerateQueryNodeModifiers(
	    node, [&](unique_ptr<ParsedExpression> &child) {
		    RequalifyColumnRefsInExpression(child, pushed_aliases, fragment_alias);
	    });
	if (node.from_table) {
		RequalifyColumnRefsInTableRef(*node.from_table, pushed_aliases, fragment_alias);
	}
}

bool RemotePushdownOptimizer::PushRemoteSubtreeGrouped(unique_ptr<TableRef> &ref, CatalogPushdownResult result,
                                                      SelectNode &node) {
	vector<std::pair<Identifier, reference<BaseTableRef>>> tables;
	if (!CollectGroupableBaseTables(*ref, tables) || tables.size() < 2) {
		return false;
	}

	// Every alias in the subtree must be distinct for the prefixed names to be unambiguous
	identifier_set_t pushed_aliases;
	for (auto &entry : tables) {
		if (!pushed_aliases.insert(entry.first).second) {
			return false;
		}
	}

	// Project "alias.column AS alias__column" for every column of every table in the
	// subtree. The flat remote result would otherwise collide on any column name the
	// tables share (order_id appears in both orders and order_items).
	vector<unique_ptr<ParsedExpression>> projection;
	// alias__column is not injective: alias `o` column `x__y` and alias `o__x` column `y` both
	// produce o__x__y. The flat result would carry the name twice and the master would bind the
	// first for both, silently reading one column's values under the other's name. Track the
	// names and decline the whole grouping on a collision - the caller then pushes each table
	// separately, which is correct and merely loses the optimization here.
	case_insensitive_set_t projected_names;
	for (auto &entry : tables) {
		auto &alias = entry.first;
		auto &base = entry.second.get();

		Identifier catalog_name;
		vector<Identifier> schema_path;
		ResolveQualification(base.GetQualifiedName(), catalog_name, schema_path);
		EntryLookupInfo lookup(CatalogType::TABLE_ENTRY, base.Table());
		auto entry_ptr = LookupEntry(catalog_name, lookup, schema_path);
		if (!entry_ptr || entry_ptr->type != CatalogType::TABLE_ENTRY) {
			return false;
		}
		auto &table_entry = entry_ptr->Cast<TableCatalogEntry>();
		auto &columns = table_entry.GetColumns();
		if (columns.LogicalColumnCount() == 0) {
			return false;
		}
		for (auto &col : columns.Logical()) {
			auto projected = alias.GetIdentifierName() + "__" + col.Name().GetIdentifierName();
			if (!projected_names.insert(projected).second) {
				return false;
			}
			vector<Identifier> qualified {alias, col.Name()};
			auto col_ref = make_uniq<ColumnRefExpression>(std::move(qualified));
			col_ref->SetAlias(Identifier(projected));
			projection.push_back(std::move(col_ref));
		}
	}

	auto filter = BuildPushableFilter(node.where_clause.get(), pushed_aliases);

	// A distinct alias per fragment keeps several grouped pushdowns in one query apart
	auto fragment_alias = Identifier("__fed_j" + std::to_string(pushdown_state.grouped_fragment_counter++));

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list = std::move(projection);
	select_node->from_table = std::move(ref);
	select_node->where_clause = std::move(filter);
	StripCatalogName(*select_node, result.catalog->GetName());

	ref = CreateRemoteFunctionRef(result, std::move(select_node));
	if (!ref) {
		return false;
	}
	ref->alias = fragment_alias;

	// The enclosing query still says o.x / oi.y - point those at the flattened result
	PreserveSelectListNames(node);
	RequalifyColumnRefs(node, pushed_aliases, fragment_alias);
	return true;
}

//===--------------------------------------------------------------------===//
// Partial aggregate pushdown
//===--------------------------------------------------------------------===//
//
// When an aggregate's inputs all come from one remote side, that side can group by the
// columns referenced above it and return partial aggregates instead of raw rows. A query
// scanning millions of rows to produce a handful then ships one row per group.
//
// Validity (Yan & Larson eager aggregation). For an equi-join on key k, take a group key
// value with a_k rows on the pushed side and b_k on the other. The original produces
// a_k * b_k pairs; pre-aggregating produces one fragment row per k which the join then
// duplicates b_k times. So the merge sees the partial b_k times:
//
//   SUM      original SUM(x) over pairs = b_k * SUM(x over a_k)
//            merged   SUM(partial) over b_k copies = b_k * SUM(x over a_k)   equal
//   COUNT(*) original a_k * b_k;  merged SUM(a_k) over b_k copies = a_k * b_k   equal
//   MIN/MAX  idempotent under duplication                                    equal
//
// The duplication factor cancels because it applies uniformly to every row of the group.
// That argument fails for anything not additive-or-idempotent, and for aggregates over the
// OTHER side: SUM(b.x) originally counts each b row a_k times, but after collapsing the
// pushed side it is counted once. Such aggregates therefore decline.
//
// NULLs survive: SUM of an all-NULL group is NULL and SUM ignores NULL partials; COUNT(x)
// counts non-NULL and sums correctly; MIN/MAX ignore NULL.
//
// COUNT(DISTINCT x) does NOT decompose. SUM(COUNT(DISTINCT x) per k) double-counts any x
// appearing under two different k. Grouping the fragment by (k, x) instead would preserve
// the distinct values, but yields one row per distinct pair - no reduction when x is
// near-unique per k - so it is not worth shipping and is declined outright.

//! Aggregates this rewrite recognises. Detection is by name because at this stage the tree is
//! unbound: ParsedExpression::IsAggregate() only recurses into children and no FunctionExpression
//! overrides it, so nothing here knows that "sum" is an aggregate rather than a scalar function.
//! Only unqualified names match, so a schema-qualified user aggregate never does.
static bool IsKnownAggregateName(const FunctionExpression &func) {
	if (!func.GetQualifiedName().Catalog().empty() || !func.GetQualifiedName().Schema().empty()) {
		return false;
	}
	auto name = StringUtil::Lower(func.FunctionName().GetIdentifierName());
	return name == "sum" || name == "count" || name == "count_star" || name == "min" || name == "max" ||
	       name == "avg" || name == "mean" || name == "median" || name == "stddev" || name == "stddev_samp" ||
	       name == "stddev_pop" || name == "var_samp" || name == "var_pop" || name == "variance" ||
	       name == "string_agg" || name == "list" || name == "array_agg" || name == "bool_and" ||
	       name == "bool_or" || name == "product" || name == "first" || name == "last" ||
	       name == "arg_min" || name == "arg_max" || name == "approx_count_distinct" || name == "quantile" ||
	       name == "quantile_cont" || name == "quantile_disc" || name == "histogram" || name == "mode" ||
	       name == "entropy" || name == "kurtosis" || name == "skewness" || name == "corr" ||
	       name == "covar_pop" || name == "covar_samp" || name == "bit_and" || name == "bit_or" ||
	       name == "bit_xor" || name == "count_if" || name == "sum_no_overflow" || name == "fsum" ||
	       name == "favg" || name == "regr_slope" || name == "regr_intercept" || name == "regr_count";
}

bool RemotePushdownOptimizer::DecomposeAggregate(const FunctionExpression &agg, string &merge_function,
                                                bool &zero_default) {
	zero_default = false;
	// DISTINCT breaks additivity: summing per-group distinct counts double-counts any value
	// that appears under more than one group key. FILTER / ORDER BY / export_state change the
	// aggregate's meaning in ways the two-phase form does not reproduce.
	if (agg.Distinct() || agg.Filter() || agg.ExportState()) {
		return false;
	}
	// FunctionExpression always allocates an OrderModifier, so an aggregate with no ORDER BY
	// still has a non-null one - only actual ordering entries disqualify
	if (agg.OrderBy() && !agg.OrderBy()->orders.empty()) {
		return false;
	}
	auto name = StringUtil::Lower(agg.FunctionName().GetIdentifierName());
	if (name == "sum" || name == "count" || name == "count_star" || name == "sum_no_overflow") {
		// Partial sums and counts are combined by summing
		merge_function = "sum";
		// COUNT and SUM differ on empty input, and the difference survives the rewrite. When a
		// pushed filter eliminates every row the fragment returns no rows at all, so the master's
		// SUM sees an empty input and yields NULL. That is right for SUM but wrong for COUNT,
		// which must be 0 - an ungrouped aggregate emits one row for the implicit group whether
		// or not any input row survived. So the count family carries a zero default and SUM
		// deliberately does not.
		zero_default = (name == "count" || name == "count_star");
		return true;
	}
	if (name == "min" || name == "max") {
		// Idempotent, so join duplication of the partial does not change the result
		merge_function = name;
		return true;
	}
	// Everything else either needs several partials (avg = sum/count) or has no additive
	// merge at all (median, stddev, string_agg, ...)
	return false;
}

//! Walk expr collecting the aggregates to push. Descends through ordinary functions so
//! ROUND(SUM(x), 2) finds the SUM, but never into an aggregate's own arguments - those are
//! consumed inside the fragment. Returns false when an aggregate is found that must not be
//! pushed, which disqualifies the whole side.
bool RemotePushdownOptimizer::CollectPushableAggregates(const ParsedExpression &expr,
                                                       const identifier_set_t &pushed_aliases, idx_t &counter,
                                                       vector<PartialAggregate> &out) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<FunctionExpression>();
		if (IsKnownAggregateName(func)) {
			// Which side supplies this aggregate's inputs?
			bool references_other = false;
			ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
			    func, [&](const ColumnRefExpression &col_ref) {
				    auto &names = col_ref.ColumnNames();
				    if (names.size() < 2 || pushed_aliases.find(names[names.size() - 2]) == pushed_aliases.end()) {
					    references_other = true;
				    }
			    });
			// An aggregate reading the other side would be computed over the collapsed row
			// count: SUM(b.x) counts each b row once instead of once per pushed-side row.
			// COUNT(*) has no column reference and legitimately counts pushed-side rows.
			if (references_other) {
				return false;
			}
			string merge_function;
			bool zero_default = false;
			if (!DecomposeAggregate(func, merge_function, zero_default)) {
				return false;
			}
			PartialAggregate agg;
			agg.original = &expr;
			agg.merge_function = std::move(merge_function);
			agg.zero_default = zero_default;
			agg.partial = func.Copy();
			// The same aggregate is often written twice - once in the select list and again in
			// HAVING or ORDER BY. Both occurrences need their own entry, because the replacement
			// pass finds them by identity, but they can share one fragment column instead of
			// computing and shipping the partial twice.
			//
			// Equality is ParsedExpression::Equals rather than a ToString() comparison: it walks
			// the tree and compares the qualified name, the arguments, DISTINCT, FILTER and ORDER
			// BY, while ignoring the alias. So "sum(o.amt) AS s" in the select list matches the
			// bare "sum(o.amt)" in HAVING, which a text comparison would not, and it cannot be
			// fooled by two different trees that happen to render alike.
			for (auto &existing : out) {
				if (existing.partial->Equals(*agg.partial)) {
					agg.partial_name = existing.partial_name;
					agg.projected = false;
					break;
				}
			}
			if (agg.projected) {
				agg.partial_name = Identifier("__fedagg_" + std::to_string(counter++));
			}
			out.push_back(std::move(agg));
			return true;
		}
	}
	// A subquery could reference either side in ways this rewrite does not model
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		return false;
	}
	bool ok = true;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!CollectPushableAggregates(child, pushed_aliases, counter, out)) {
			ok = false;
		}
	});
	return ok;
}

bool RemotePushdownOptimizer::AllJoinsAreInner(const TableRef &ref) {
	if (ref.type != TableReferenceType::JOIN) {
		return true;
	}
	auto &join = ref.Cast<JoinRef>();
	if (join.type != JoinType::INNER || join.ref_type != JoinRefType::REGULAR) {
		return false;
	}
	if (join.left && !AllJoinsAreInner(*join.left)) {
		return false;
	}
	if (join.right && !AllJoinsAreInner(*join.right)) {
		return false;
	}
	return true;
}

bool RemotePushdownOptimizer::NodeHasWindow(const SelectNode &node) {
	for (auto &expr : node.select_list) {
		if (expr->IsWindow()) {
			return true;
		}
	}
	if (node.having && node.having->IsWindow()) {
		return true;
	}
	for (auto &modifier : node.modifiers) {
		if (modifier->type != ResultModifierType::ORDER_MODIFIER) {
			continue;
		}
		for (auto &order : modifier->Cast<OrderModifier>().orders) {
			if (order.expression->IsWindow()) {
				return true;
			}
		}
	}
	return false;
}

bool RemotePushdownOptimizer::AddGroupColumn(vector<GroupColumn> &out, const Identifier &alias,
                                             const Identifier &column) {
	// Record once, preserving first-seen order so the fragment's projection is deterministic
	for (auto &existing : out) {
		if (existing.alias == alias && existing.column == column) {
			return true;
		}
	}
	auto projected = Identifier(alias.GetIdentifierName() + "__" + column.GetIdentifierName());
	// alias__column is not injective: alias `o` column `x__y` and alias `o__x` column `y` both
	// produce o__x__y. Projecting the name twice makes the second silently shadow the first, so
	// the master reads one column's values under the other's name. Decline the side instead -
	// the caller falls back to pushing each table separately, which is correct and only loses
	// the grouping optimization for this query.
	for (auto &existing : out) {
		if (existing.projected_name == projected) {
			return false;
		}
	}
	GroupColumn col;
	col.alias = alias;
	col.column = column;
	col.projected_name = std::move(projected);
	out.push_back(std::move(col));
	return true;
}

bool RemotePushdownOptimizer::CollectPushedColumns(const ParsedExpression &expr,
                                                  const identifier_set_t &pushed_aliases,
                                                  const vector<PartialAggregate> &aggregates,
                                                  vector<GroupColumn> &out) {
	// An aggregate being pushed consumes its own columns inside the fragment - they do not
	// have to survive as group keys
	for (auto &agg : aggregates) {
		if (agg.original == &expr) {
			return true;
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &col_ref = expr.Cast<ColumnRefExpression>();
		auto &names = col_ref.ColumnNames();
		if (names.size() < 2) {
			// Unqualified: cannot be attributed to a side before binding. Only a problem if
			// it might belong to the pushed side, which we cannot rule out.
			return false;
		}
		auto &qualifier = names[names.size() - 2];
		if (pushed_aliases.find(qualifier) != pushed_aliases.end()) {
			// A projected-name collision disqualifies the side; see AddGroupColumn
			if (!AddGroupColumn(out, qualifier, names.back())) {
				return false;
			}
		}
		return true;
	}
	// A subquery may reference the pushed side in ways this rewrite does not track
	if (expr.HasSubquery()) {
		return false;
	}
	bool ok = true;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		if (!CollectPushedColumns(child, pushed_aliases, aggregates, out)) {
			ok = false;
		}
	});
	return ok;
}

bool RemotePushdownOptimizer::CollectPushedColumnsInTableRef(const TableRef &ref,
                                                            const identifier_set_t &pushed_aliases,
                                                            const vector<PartialAggregate> &aggregates,
                                                            vector<GroupColumn> &out) {
	if (ref.type != TableReferenceType::JOIN) {
		return true;
	}
	auto &join = ref.Cast<JoinRef>();
	if (join.condition && !CollectPushedColumns(*join.condition, pushed_aliases, aggregates, out)) {
		return false;
	}
	// USING columns are unqualified by construction, so they could resolve to either side.
	// Treat any USING as disqualifying rather than guessing.
	if (!join.using_columns.empty()) {
		return false;
	}
	if (join.left && !CollectPushedColumnsInTableRef(*join.left, pushed_aliases, aggregates, out)) {
		return false;
	}
	if (join.right && !CollectPushedColumnsInTableRef(*join.right, pushed_aliases, aggregates, out)) {
		return false;
	}
	return true;
}

void RemotePushdownOptimizer::CollectSelectListAliases(const SelectNode &node,
                                                       identifier_map_t<const ParsedExpression *> &out) {
	// The same loop BindSelectNode runs to fill SelectBindState::alias_map: only an explicit
	// alias counts, and a repeated alias overwrites the earlier entry, because the last one is
	// what ORDER BY binds to.
	for (auto &expr : node.select_list) {
		if (expr->GetAlias().empty()) {
			continue;
		}
		out[expr->GetAlias()] = expr.get();
	}
}

const ParsedExpression &
RemotePushdownOptimizer::ResolveOrderByAlias(const ParsedExpression &expr,
                                             const identifier_map_t<const ParsedExpression *> &select_aliases) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return expr;
	}
	auto &names = expr.Cast<ColumnRefExpression>().ColumnNames();
	// Only the ORDER BY entry itself, and only a bare one-part name. OrderBinder::Bind consults
	// the select list before qualifying anything, so there an alias beats a table column of the
	// same name - but only for an entry that IS the column reference. A reference nested inside
	// a larger expression takes the general path, which qualifies first, so the table column
	// wins instead. Resolving one with the other's rule would silently reorder the result.
	if (names.size() != 1) {
		return expr;
	}
	auto entry = select_aliases.find(names[0]);
	if (entry == select_aliases.end()) {
		// A genuinely unqualified column, which still cannot be attributed to a side here
		return expr;
	}
	// One step, as the binder does: the aliased expression is attributed by the ordinary rules
	// and its own names are not resolved as aliases again. So `SELECT a + 1 AS a ORDER BY a`
	// lands on `a + 1` and the `a` inside it is treated as a column, and resolution cannot cycle.
	return *entry->second;
}

bool RemotePushdownOptimizer::PlanPartialAggregate(const SelectNode &node, const identifier_set_t &pushed_aliases,
                                                  PartialAggregatePlan &plan) {
	// GROUPING SETS / ROLLUP / CUBE produce several groupings at once; the two-phase rewrite
	// below assumes a single one
	if (node.groups.grouping_sets.size() > 1) {
		return false;
	}
	// QUALIFY and window functions both need per-row detail that aggregation destroys
	if (node.qualify || NodeHasWindow(node)) {
		return false;
	}
	// Pre-aggregation changes the multiplicity a NULL-extending join observes
	if (!node.from_table || !AllJoinsAreInner(*node.from_table)) {
		return false;
	}

	// Collect every aggregate. All must be over the pushed side and decomposable: one
	// aggregate over the other side would be computed over the collapsed row count and
	// silently wrong, so a single unsuitable aggregate disqualifies the whole side.
	idx_t partial_counter = 0;
	for (auto &expr : node.select_list) {
		if (!CollectPushableAggregates(*expr, pushed_aliases, partial_counter, plan.aggregates)) {
			return false;
		}
	}
	// HAVING and ORDER BY may hold aggregates too; they get the same merge treatment, so they
	// must be collected here rather than left behind referencing columns the fragment removed
	if (node.having && !CollectPushableAggregates(*node.having, pushed_aliases, partial_counter, plan.aggregates)) {
		return false;
	}
	for (auto &modifier : node.modifiers) {
		if (modifier->type != ResultModifierType::ORDER_MODIFIER) {
			continue;
		}
		for (auto &order : modifier->Cast<OrderModifier>().orders) {
			if (!CollectPushableAggregates(*order.expression, pushed_aliases, partial_counter, plan.aggregates)) {
				return false;
			}
		}
	}
	if (plan.aggregates.empty()) {
		// Nothing to pre-aggregate; a plain scan pushdown is the right treatment
		return false;
	}

	// Every other pushed-side reference must survive aggregation as a group key
	for (auto &expr : node.select_list) {
		if (!CollectPushedColumns(*expr, pushed_aliases, plan.aggregates, plan.group_columns)) {
			return false;
		}
	}
	for (auto &expr : node.groups.group_expressions) {
		if (!CollectPushedColumns(*expr, pushed_aliases, plan.aggregates, plan.group_columns)) {
			return false;
		}
	}
	if (node.having && !CollectPushedColumns(*node.having, pushed_aliases, plan.aggregates, plan.group_columns)) {
		return false;
	}
	// An ORDER BY entry naming one of this select list's own aliases is not a column at all, so
	// it is resolved before attribution. The columns that decide the side are the aliased
	// expression's, and every select-list expression has already been walked above.
	identifier_map_t<const ParsedExpression *> select_aliases;
	CollectSelectListAliases(node, select_aliases);
	for (auto &modifier : node.modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			for (auto &order : modifier->Cast<OrderModifier>().orders) {
				auto &target = ResolveOrderByAlias(*order.expression, select_aliases);
				if (!CollectPushedColumns(target, pushed_aliases, plan.aggregates, plan.group_columns)) {
					return false;
				}
			}
		} else if (modifier->type == ResultModifierType::DISTINCT_MODIFIER) {
			for (auto &expr : modifier->Cast<DistinctModifier>().distinct_on_targets) {
				if (!CollectPushedColumns(*expr, pushed_aliases, plan.aggregates, plan.group_columns)) {
					return false;
				}
			}
		}
	}
	// Join keys must survive so the master can still join the fragment
	if (!CollectPushedColumnsInTableRef(*node.from_table, pushed_aliases, plan.aggregates, plan.group_columns)) {
		return false;
	}
	// The WHERE clause is the one place references may vanish: conjuncts confined to the
	// pushed side travel into the fragment and are dropped from the master. Anything else
	// touching the pushed side would have to be evaluated after aggregation.
	if (node.where_clause) {
		vector<reference<ParsedExpression>> conjuncts;
		CollectConjuncts(const_cast<ParsedExpression &>(*node.where_clause), conjuncts);
		for (auto &conjunct : conjuncts) {
			if (CanPushConjunctTo(conjunct.get(), pushed_aliases)) {
				continue; // travels with the fragment
			}
			// Stays at the master, so any pushed-side column it needs must be a group key
			if (!CollectPushedColumns(conjunct.get(), pushed_aliases, plan.aggregates, plan.group_columns)) {
				return false;
			}
		}
	}
	// Grouping by nothing would collapse the side to one row and lose the join key
	return !plan.group_columns.empty();
}

void RemotePushdownOptimizer::ApplyPartialAggregatesInExpression(unique_ptr<ParsedExpression> &expr,
                                                                const PartialAggregatePlan &plan,
                                                                const Identifier &fragment_alias) {
	if (!expr) {
		return;
	}
	for (auto &agg : plan.aggregates) {
		if (agg.original != expr.get()) {
			continue;
		}
		// SUM(oi.x) becomes sum(__fed_aN.__fedagg_0)
		vector<Identifier> qualified {fragment_alias, agg.partial_name};
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(make_uniq<ColumnRefExpression>(std::move(qualified)));
		unique_ptr<ParsedExpression> merged =
		    make_uniq<FunctionExpression>(Identifier(agg.merge_function), std::move(children));
		if (agg.zero_default) {
			// COUNT(*) becomes COALESCE(sum(__fed_aN.__fedagg_0), 0). Without this an ungrouped
			// count whose fragment returned no rows yields NULL, because the merge is SUM. The
			// coalesce is a no-op whenever any partial row exists, so it only affects the empty
			// case it exists for.
			//
			// COALESCE is an operator in DuckDB, not a catalog scalar function - building it as a
			// FunctionExpression fails to bind with "Scalar Function with name coalesce does not exist".
			auto coalesce = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
			coalesce->GetChildrenMutable().push_back(std::move(merged));
			coalesce->GetChildrenMutable().push_back(make_uniq<ConstantExpression>(Value::BIGINT(0)));
			merged = std::move(coalesce);
		}
		// Keep the original output name so the result column is unchanged
		merged->SetAlias(expr->GetAlias());
		expr = std::move(merged);
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ApplyPartialAggregatesInExpression(child, plan, fragment_alias); });
}

void RemotePushdownOptimizer::ApplyPartialAggregates(SelectNode &node, const PartialAggregatePlan &plan,
                                                    const Identifier &fragment_alias) {
	for (auto &expr : node.select_list) {
		ApplyPartialAggregatesInExpression(expr, plan, fragment_alias);
	}
	if (node.having) {
		ApplyPartialAggregatesInExpression(node.having, plan, fragment_alias);
	}
	for (auto &modifier : node.modifiers) {
		if (modifier->type != ResultModifierType::ORDER_MODIFIER) {
			continue;
		}
		for (auto &order : modifier->Cast<OrderModifier>().orders) {
			ApplyPartialAggregatesInExpression(order.expression, plan, fragment_alias);
		}
	}
}

void RemotePushdownOptimizer::PreserveSelectListNames(SelectNode &node) {
	// Requalification rewrites "p"."category" to "__fed_aN"."p__category", and an unaliased
	// select item takes its output name from the column it references - so without pinning the
	// name first the result column would come back as p__category instead of category.
	for (auto &expr : node.select_list) {
		if (expr->GetExpressionClass() != ExpressionClass::COLUMN_REF || !expr->GetAlias().empty()) {
			continue;
		}
		auto &names = expr->Cast<ColumnRefExpression>().ColumnNames();
		if (!names.empty()) {
			expr->SetAlias(names.back());
		}
	}
}

void RemotePushdownOptimizer::RemovePushedConjuncts(SelectNode &node, const identifier_set_t &pushed_aliases) {
	if (!node.where_clause) {
		return;
	}
	vector<reference<ParsedExpression>> conjuncts;
	CollectConjuncts(*node.where_clause, conjuncts);
	// Rebuild the WHERE from the conjuncts that did NOT travel with the fragment
	unique_ptr<ParsedExpression> kept;
	for (auto &conjunct : conjuncts) {
		if (CanPushConjunctTo(conjunct.get(), pushed_aliases)) {
			continue;
		}
		auto copy = conjunct.get().Copy();
		if (!kept) {
			kept = std::move(copy);
		} else {
			kept = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(kept), std::move(copy));
		}
	}
	node.where_clause = std::move(kept);
}

bool RemotePushdownOptimizer::PushRemoteSubtreeAggregated(unique_ptr<TableRef> &ref, CatalogPushdownResult result,
                                                          SelectNode &node) {
	if (!ref) {
		return false;
	}
	// The side must be something the source can group: a base table, or an inner-join subtree
	if (ref->type == TableReferenceType::JOIN && !AllJoinsAreInner(*ref)) {
		return false;
	}

	identifier_set_t pushed_aliases;
	CollectTableAliases(*ref, pushed_aliases);
	if (pushed_aliases.empty()) {
		return false;
	}

	PartialAggregatePlan plan;
	if (!PlanPartialAggregate(node, pushed_aliases, plan)) {
		return false;
	}

	// SELECT <group cols AS alias__col>, <partial aggs AS __fedagg_N>
	// FROM <side> WHERE <pushable conjuncts> GROUP BY <group cols>
	vector<unique_ptr<ParsedExpression>> projection;
	vector<unique_ptr<ParsedExpression>> group_expressions;
	for (auto &col : plan.group_columns) {
		vector<Identifier> qualified {col.alias, col.column};
		auto projected = make_uniq<ColumnRefExpression>(qualified);
		projected->SetAlias(col.projected_name);
		projection.push_back(std::move(projected));
		group_expressions.push_back(make_uniq<ColumnRefExpression>(std::move(qualified)));
	}
	for (auto &agg : plan.aggregates) {
		if (!agg.projected) {
			// A repeat of an aggregate already in the projection; both merge the same column
			continue;
		}
		auto partial = agg.partial->Copy();
		partial->SetAlias(agg.partial_name);
		projection.push_back(std::move(partial));
	}

	auto filter = BuildPushableFilter(node.where_clause.get(), pushed_aliases);
	auto fragment_alias = Identifier("__fed_a" + std::to_string(pushdown_state.grouped_fragment_counter++));

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list = std::move(projection);
	select_node->from_table = std::move(ref);
	select_node->where_clause = std::move(filter);
	select_node->groups.group_expressions = std::move(group_expressions);
	// SelectNode::ToString() renders GROUP BY from grouping_sets, not from group_expressions,
	// and the fragment travels as SQL text - without this the clause is silently dropped and
	// the source rejects the query for selecting a non-grouped column
	GroupingSet grouping_set;
	for (idx_t i = 0; i < select_node->groups.group_expressions.size(); i++) {
		grouping_set.insert(ProjectionIndex(i));
	}
	select_node->groups.grouping_sets.push_back(std::move(grouping_set));
	StripCatalogName(*select_node, result.catalog->GetName());

	ref = CreateRemoteFunctionRef(result, std::move(select_node));
	if (!ref) {
		return false;
	}
	ref->alias = fragment_alias;

	// The conjuncts that travelled reference columns aggregation has removed, so unlike a
	// plain scan pushdown they cannot be left behind. Safe because the join is inner.
	RemovePushedConjuncts(node, pushed_aliases);
	// Pin output names before requalification renames the columns they derive from
	PreserveSelectListNames(node);
	// Aggregates become merges over the partial columns
	ApplyPartialAggregates(node, plan, fragment_alias);
	// Surviving pushed-side columns now live under the fragment's flattened names
	RequalifyColumnRefs(node, pushed_aliases, fragment_alias);
	return true;
}

void RemotePushdownOptimizer::StripCatalogPrefix(ParsedExpression &expr, const Identifier &catalog_name) {
	// Unlike StripCatalogName, which normalises to exactly table.column because the whole
	// statement is moving to the source, this removes only the leading catalog identifier. The
	// rest of the path has to survive: `rpc.t.s.a.b.c` is a struct walk, and taking the last two
	// names would leave `b.c` and lose the column.
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(
	    expr, [&](ColumnRefExpression &col_ref) {
		    auto &names = col_ref.ColumnNamesMutable();
		    if (names.size() >= 3 && names[0] == catalog_name) {
			    names.erase(names.begin());
		    }
	    });
}

void RemotePushdownOptimizer::StripCatalogFromJoinConditions(optional_ptr<TableRef> ref,
                                                             const Identifier &catalog_name) {
	if (!ref || ref->type != TableReferenceType::JOIN) {
		return;
	}
	auto &join = ref->Cast<JoinRef>();
	if (join.condition) {
		StripCatalogPrefix(*join.condition, catalog_name);
	}
	StripCatalogFromJoinConditions(join.left.get(), catalog_name);
	StripCatalogFromJoinConditions(join.right.get(), catalog_name);
}

void RemotePushdownOptimizer::StripCatalogFromNodeExpressions(SelectNode &node, const Identifier &catalog_name) {
	// Only the expressions. The FROM tree's own table names are left alone, because a table of
	// this catalog that was not pushed still has to resolve through its catalog.
	for (auto &expr : node.select_list) {
		if (expr) {
			StripCatalogPrefix(*expr, catalog_name);
		}
	}
	if (node.where_clause) {
		StripCatalogPrefix(*node.where_clause, catalog_name);
	}
	for (auto &expr : node.groups.group_expressions) {
		if (expr) {
			StripCatalogPrefix(*expr, catalog_name);
		}
	}
	if (node.having) {
		StripCatalogPrefix(*node.having, catalog_name);
	}
	if (node.qualify) {
		StripCatalogPrefix(*node.qualify, catalog_name);
	}
	for (auto &modifier : node.modifiers) {
		if (!modifier || modifier->type != ResultModifierType::ORDER_MODIFIER) {
			continue;
		}
		for (auto &order : modifier->Cast<OrderModifier>().orders) {
			if (order.expression) {
				StripCatalogPrefix(*order.expression, catalog_name);
			}
		}
	}
	StripCatalogFromJoinConditions(node.from_table.get(), catalog_name);
}

void RemotePushdownOptimizer::PushCrossCatalogJoinSides(SelectNode &node, idx_t pending_base) {
	// Only the entries this SelectNode recorded - earlier ones belong to enclosing scopes
	auto &pending_sides = pushdown_state.pending_join_sides;
	for (idx_t i = pending_base; i < pending_sides.size(); i++) {
		auto &pending = pending_sides[i];
		if (!pending.ref_slot || !*pending.ref_slot) {
			continue;
		}
		// Pre-aggregating at the source turns a full-table scan into one row per group, so
		// try it before falling back to shipping rows
		if (PushRemoteSubtreeAggregated(*pending.ref_slot, pending.result, node)) {
			continue;
		}
		PushRemoteSubtreeTables(*pending.ref_slot, pending.result, node);
		// The side no longer resolves through its catalog: it is a fragment now, bound under the
		// table's own alias. A conjunct BuildPushableFilter left behind, or an outer ON or select
		// list entry, still says `catalog.table.column` and would fail to bind. Normalising those
		// to `table.column` is what upstream does for whole-statement pushdown.
		if (pending.result.catalog) {
			StripCatalogFromNodeExpressions(node, pending.result.catalog->GetName());
		}
	}
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteNode(InsertQueryNode &node) {
	// first bind the target table for the insert
	BaseTableRef target_ref;
	target_ref.SetQualifiedName(node.qualified_name);

	RemotePushdownOptimizer target_optimizer(this);
	auto result = target_optimizer.Rewrite(target_ref);
	if (node.select_statement) {
		RemotePushdownOptimizer select_optimizer(this);
		auto select_result = select_optimizer.Rewrite(*node.select_statement->node);
		result = Merge(result, select_result);
		if (select_result.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
			bool push_select_only = result.reference_type != CatalogReferenceType::SINGLE_REMOTE_CATALOG;
			if (!push_select_only && !result.catalog->SupportsPushdown(node)) {
				// the catalog cannot execute the INSERT itself remotely - push down only the SELECT part
				push_select_only = true;
			}
			if (push_select_only) {
				FinishPushdown(node.select_statement->node, select_result);
			}
		}
	}
	if (node.on_conflict_info) {
		if (node.on_conflict_info->condition) {
			auto condition_result = Rewrite(node.on_conflict_info->condition);
			result = Merge(result, condition_result);
		}
		if (node.on_conflict_info->set_info) {
			if (node.on_conflict_info->set_info->condition) {
				auto condition_result = Rewrite(node.on_conflict_info->set_info->condition);
				result = Merge(result, condition_result);
			}
			for (auto &expr : node.on_conflict_info->set_info->expressions) {
				auto expr_result = Rewrite(expr);
				result = Merge(result, expr_result);
			}
		}
	}
	for (auto &expr : node.returning_list) {
		auto expr_result = Rewrite(expr);
		result = Merge(result, expr_result);
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteNode(DeleteQueryNode &node) {
	auto result = Rewrite(node.table);
	vector<CatalogPushdownResult> using_results;
	for (auto &using_clause : node.using_clauses) {
		auto using_result = Rewrite(using_clause);
		using_results.push_back(using_result);
		result = Merge(result, using_result);
	}

	if (node.condition) {
		auto condition_result = Rewrite(node.condition);
		result = Merge(result, condition_result);
	}
	for (auto &expr : node.returning_list) {
		auto expr_result = Rewrite(expr);
		result = Merge(result, expr_result);
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteNode(UpdateQueryNode &node) {
	auto result = Rewrite(node.table);
	auto from_result = CatalogPushdownResult::NoCatalogReference();
	if (node.from_table) {
		from_result = Rewrite(node.from_table);
		result = Merge(result, from_result);
	}

	if (node.set_info) {
		if (node.set_info->condition) {
			auto condition_result = Rewrite(node.set_info->condition);
			result = Merge(result, condition_result);
		}

		for (auto &expr : node.set_info->expressions) {
			auto expr_result = Rewrite(expr);
			result = Merge(result, expr_result);
		}
	}
	for (auto &expr : node.returning_list) {
		auto expr_result = Rewrite(expr);
		result = Merge(result, expr_result);
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteNode(MergeQueryNode &node) {
	// the target and the source form a join - like UPDATE ... FROM they are analyzed in the same scope,
	// so a local table on either side is tracked for the action expressions below
	auto result = Rewrite(node.target);
	result = Merge(result, Rewrite(node.source));
	if (node.join_condition) {
		result = Merge(result, Rewrite(node.join_condition));
	}
	for (auto &entry : node.actions) {
		for (auto &action : entry.second) {
			if (action->condition) {
				result = Merge(result, Rewrite(action->condition));
			}
			if (action->update_info) {
				if (action->update_info->condition) {
					result = Merge(result, Rewrite(action->update_info->condition));
				}
				for (auto &expr : action->update_info->expressions) {
					result = Merge(result, Rewrite(expr));
				}
			}
			for (auto &expr : action->expressions) {
				result = Merge(result, Rewrite(expr));
			}
		}
	}
	for (auto &expr : node.returning_list) {
		result = Merge(result, Rewrite(expr));
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteNode(SetOperationNode &node) {
	// Rewrite each child independently so we can push down individual children if needed
	vector<CatalogPushdownResult> child_results;
	child_results.reserve(node.children.size());
	auto result = CatalogPushdownResult::NoCatalogReference();
	for (auto &child : node.children) {
		RemotePushdownOptimizer child_optimizer(this);
		auto child_result = child_optimizer.Rewrite(*child);
		result = Merge(result, child_result);
		child_results.push_back(child_result);
	}

	// Check result modifiers (ORDER BY / LIMIT on the set operation itself)
	bool has_expression_modifiers = false;
	for (auto &modifier : node.modifiers) {
		switch (modifier->type) {
		case ResultModifierType::ORDER_MODIFIER: {
			auto &order_mod = modifier->Cast<OrderModifier>();
			for (auto &order : order_mod.orders) {
				// ORDER BY entries cannot be constant-folded - a bare integer literal is a
				// positional reference there
				result = Merge(result, Rewrite(order.expression, ExpressionFoldingMode::FOLD_CHILDREN_ONLY));
				has_expression_modifiers = true;
			}
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER: {
			auto &limit_mod = modifier->Cast<LimitModifier>();
			if (limit_mod.limit) {
				result = Merge(result, Rewrite(limit_mod.limit));
				has_expression_modifiers = true;
			}
			if (limit_mod.offset) {
				result = Merge(result, Rewrite(limit_mod.offset));
				has_expression_modifiers = true;
			}
			break;
		}
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &distinct_mod = modifier->Cast<DistinctModifier>();
			for (auto &expr : distinct_mod.distinct_on_targets) {
				// DISTINCT ON entries cannot be constant-folded - a bare integer literal is a
				// positional reference there
				result = Merge(result, Rewrite(expr, ExpressionFoldingMode::FOLD_CHILDREN_ONLY));
				has_expression_modifiers = true;
			}
			break;
		}
		default:
			break;
		}
	}
	// If the whole set operation resolves to a single remote catalog, propagate upward.
	if (result.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		return result;
	}
	if (has_expression_modifiers) {
		// if the set operation has any modifiers (e.g. ORDER BY <expr>) then binding can go wrong if we do a pushdown
		// into children, since we might have something like SELECT i + 1 FROM remote UNION ALL ... ORDER BY i + 1
		// this requires "peeking into" the child query to figure out that the expressions match
		// for now just be safe and skip pushdown into individual queries in this scenario
		return result;
	}
	for (idx_t i = 0; i < node.children.size(); i++) {
		FinishPushdown(node.children[i], child_results[i]);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// DDL statements
//===--------------------------------------------------------------------===//
CatalogPushdownResult RemotePushdownOptimizer::ResolveDDLTarget(const QualifiedName &name, DDLTarget target,
                                                                CatalogType entry_type) {
	Identifier catalog_name;
	vector<Identifier> schema_path;
	ResolveQualification(name, catalog_name, schema_path);
	if (!catalog_name.empty()) {
		return ResolveRemoteCatalog(catalog_name, RemoteCapability::EXECUTE_STATEMENT);
	}
	// no explicit catalog - the statement is only pushed down if the search path resolves it to a remote
	FindRemoteCatalogsInSearchPath();
	if (pushdown_state.remote_catalogs_in_search_path.size() != 1) {
		return CatalogPushdownResult::Unknown();
	}
	auto &remote_catalog = pushdown_state.remote_catalogs_in_search_path.front().get();
	if (target == DDLTarget::NEW_ENTRY) {
		// the entry does not exist yet - it is created in the catalog Binder::SearchSchema would pick
		auto &search_path = *ClientData::Get(binder.context).catalog_search_path;
		auto resolved = schema_path.empty() ? search_path.GetDefault().GetCatalog()
		                                    : search_path.GetDefaultCatalog(schema_path.front());
		if (resolved != remote_catalog.GetName()) {
			return CatalogPushdownResult::Unknown();
		}
		return ResolveRemoteCatalog(remote_catalog.GetName(), RemoteCapability::EXECUTE_STATEMENT);
	}
	if (entry_type == CatalogType::SCHEMA_ENTRY) {
		// a schema is not looked up as a (catalog, schema, name) triple - only push DROP/ALTER SCHEMA
		// when the catalog is named explicitly
		return CatalogPushdownResult::Unknown();
	}
	// the entry must already exist - if any local catalog in the search path holds it, stay local
	EntryLookupInfo entry_lookup(entry_type, QualifiedName(name.Name()));
	if (EntryExistsInLocalCatalog(entry_lookup, schema_path)) {
		return CatalogPushdownResult::Unknown();
	}
	return ResolveRemoteCatalog(remote_catalog.GetName(), RemoteCapability::EXECUTE_STATEMENT);
}

CatalogPushdownResult RemotePushdownOptimizer::VerifyStatementSupport(const SQLStatement &statement,
                                                                      CatalogPushdownResult target) {
	if (target.reference_type != CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		return target;
	}
	if (!target.catalog->SupportsPushdown(statement)) {
		// the catalog cannot execute this statement as a whole - a definition query within it may still
		// be pushed on its own, so this is resolved before the statement's contents are analyzed
		return CatalogPushdownResult::Unknown();
	}
	return target;
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteCreateInfo(CreateInfo &info,
                                                                 const CatalogPushdownResult &target) {
	if (info.type != CatalogType::TABLE_ENTRY) {
		return CatalogPushdownResult::NoCatalogReference();
	}
	auto &table_info = info.Cast<CreateTableInfo>();
	if (!table_info.query) {
		return CatalogPushdownResult::NoCatalogReference();
	}
	// CREATE TABLE AS - the query is evaluated once, so it is analyzed like any other query. Everything
	// else a CREATE carries (column types and defaults, constraints, view / macro bodies, ...) is shipped
	// verbatim: whether the statement can be pushed is decided by the target catalog alone
	RemotePushdownOptimizer child_optimizer(this);
	auto result = child_optimizer.Rewrite(*table_info.query->node);
	if (result.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG &&
	    (target.reference_type != CatalogReferenceType::SINGLE_REMOTE_CATALOG || target.catalog != result.catalog)) {
		// the table itself is not created in the remote catalog - push down only the query that fills it
		FinishPushdown(table_info.query->node, result);
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteStatement(CreateStatement &statement) {
	auto &info = *statement.info;
	CatalogPushdownResult target;
	switch (info.type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::SCHEMA_ENTRY:
	case CatalogType::INDEX_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		break;
	default:
		// an entry type that never lives in a remote catalog (secrets, ...)
		return CatalogPushdownResult::Unknown();
	}
	if (info.temporary) {
		// temporary entries always live in the local temp catalog
		target = CatalogPushdownResult::Unknown();
	} else if (info.type == CatalogType::SCHEMA_ENTRY) {
		// CREATE SCHEMA stores its name as [catalog, parent schemas..., new schema, <empty name>], so the
		// generic Catalog()/Schema() split does not apply to it
		auto &schema_info = info.Cast<CreateSchemaInfo>();
		target = ResolveDDLTarget(QualifiedName(schema_info.SchemaCatalog(), Identifier(), schema_info.SchemaName()),
		                          DDLTarget::NEW_ENTRY, info.type);
	} else {
		target = ResolveDDLTarget(info.GetQualifiedName(), DDLTarget::NEW_ENTRY, info.type);
	}
	target = VerifyStatementSupport(statement, std::move(target));
	return Merge(target, RewriteCreateInfo(info, target));
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteStatement(DropStatement &statement) {
	auto &info = *statement.info;
	switch (info.type) {
	case CatalogType::TABLE_ENTRY:
	case CatalogType::VIEW_ENTRY:
	case CatalogType::SCHEMA_ENTRY:
	case CatalogType::INDEX_ENTRY:
	case CatalogType::SEQUENCE_ENTRY:
	case CatalogType::TYPE_ENTRY:
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY:
		break;
	default:
		// prepared statements, secrets, ... never live in a remote catalog
		return CatalogPushdownResult::Unknown();
	}
	auto target = ResolveDDLTarget(info.GetQualifiedName(), DDLTarget::EXISTING_ENTRY, info.type);
	return VerifyStatementSupport(statement, std::move(target));
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteStatement(AlterStatement &statement) {
	auto &info = *statement.info;
	switch (info.type) {
	case AlterType::ALTER_TABLE:
	case AlterType::ALTER_VIEW:
	case AlterType::ALTER_SEQUENCE:
	case AlterType::CHANGE_OWNERSHIP:
	case AlterType::SET_COMMENT:
	case AlterType::SET_COLUMN_COMMENT:
		break;
	case AlterType::ALTER_DATABASE:
		// renaming a database renames the attachment, which only exists locally
		return CatalogPushdownResult::Unknown();
	default:
		// ALTER_SCALAR_FUNCTION / ALTER_TABLE_FUNCTION are only built by CreateInfo::GetAlterInfo when a
		// CREATE resolves an OnCreateConflict, so they never reach the optimizer as a parsed statement
		return CatalogPushdownResult::Unknown();
	}
	// COMMENT ON COLUMN targets either a table or a view, the exact type is only resolved at bind time
	auto entry_type = info.type == AlterType::SET_COLUMN_COMMENT ? CatalogType::TABLE_ENTRY : info.GetCatalogType();
	auto target = ResolveDDLTarget(info.GetQualifiedName(), DDLTarget::EXISTING_ENTRY, entry_type);
	return VerifyStatementSupport(statement, std::move(target));
}

void RemotePushdownOptimizer::TrackLocalTable(const TableRef &ref) {
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE:
		TrackLocalTable(ref.Cast<BaseTableRef>());
		break;
	case TableReferenceType::TABLE_FUNCTION:
		TrackLocalTable(ref.Cast<TableFunctionRef>());
		break;
	case TableReferenceType::SUBQUERY:
		TrackLocalTable(ref.Cast<SubqueryRef>());
		break;
	default:
		break;
	}
}

CatalogPushdownResult RemotePushdownOptimizer::Rewrite(unique_ptr<TableRef> &ref) {
	CatalogPushdownResult result;
	switch (ref->type) {
	case TableReferenceType::BASE_TABLE:
		result = Rewrite(ref->Cast<BaseTableRef>());
		break;
	case TableReferenceType::JOIN:
		result = Rewrite(ref->Cast<JoinRef>());
		break;
	case TableReferenceType::SUBQUERY:
		result = Rewrite(ref->Cast<SubqueryRef>());
		break;
	case TableReferenceType::EXPRESSION_LIST:
		result = Rewrite(ref->Cast<ExpressionListRef>());
		break;
	case TableReferenceType::TABLE_FUNCTION:
		result = Rewrite(ref->Cast<TableFunctionRef>());
		break;
	case TableReferenceType::EMPTY_FROM:
	case TableReferenceType::COLUMN_DATA:
		result = CatalogPushdownResult::NoCatalogReference();
		break;
	default:
		return CatalogPushdownResult::Unknown();
	}
	if (result.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		// the table reference is fully remote - check if the remote catalog supports pushing it down
		// (e.g. DuckDB-specific join types or TABLESAMPLE clauses cannot be sent to most remotes)
		if (!result.catalog->SupportsPushdown(*ref)) {
			TrackLocalTable(*ref);
			return CatalogPushdownResult::Unknown();
		}
	} else if (result.reference_type == CatalogReferenceType::NO_CATALOG_REFERENCED) {
		// record the table reference so a remote catalog can veto it during a later merge
		result.used_table_constructs.push_back(*ref);
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::Rewrite(ExpressionListRef &ref) {
	auto result = CatalogPushdownResult::NoCatalogReference();
	for (auto &row : ref.values) {
		for (auto &expr : row) {
			result = Merge(result, Rewrite(expr));
		}
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::Rewrite(SubqueryRef &ref) {
	RemotePushdownOptimizer child_binder(this);
	auto result = child_binder.Rewrite(*ref.subquery->node);
	if (result.reference_type == CatalogReferenceType::UNKNOWN_CATALOG_REFERENCE) {
		TrackLocalTable(ref);
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteTableFunctionOnly(TableFunctionRef &ref) {
	if (ref.function->GetExpressionClass() != ExpressionClass::FUNCTION) {
		throw InternalException("RemotePushdownOptimizer: TableFunctionRef does not hold a function expression");
	}
	auto &func_expr = ref.function->Cast<FunctionExpression>();

	// Figure out
	Identifier catalog_name;
	vector<Identifier> schema_path;
	ResolveQualification(func_expr.GetQualifiedName(), catalog_name, schema_path);

	// If the function has an explicit catalog prefix, check if it's remote
	if (!catalog_name.empty()) {
		auto catalog = Catalog::GetCatalogEntry(binder.context, catalog_name);
		if (catalog && catalog->Supports(RemoteCapability::EXECUTE_QUERY_NODE) && catalog->SupportsPushdown(ref)) {
			// "catalog" is remote and we can pushdown this function
			return CatalogPushdownResult::RemoteReference(*catalog);
		}
		// catalog was not found or catalog does not support pushdown - bail on pushdown for now
		TrackLocalTable(ref);
		return CatalogPushdownResult::Unknown();
	}

	// we have an unqualified table function
	// this function can either live in a local / system catalog, or in a remote (if it is in the search path)
	// check the search path
	FindRemoteCatalogsInSearchPath();
	EntryLookupInfo func_lookup(CatalogType::TABLE_FUNCTION_ENTRY, QualifiedName(func_expr.FunctionName()));
	for (auto &local_entry : pushdown_state.local_catalogs_in_search_path) {
		vector<Identifier> schema = schema_path.empty() ? vector<Identifier> {local_entry.GetSchema()} : schema_path;
		auto entry = LookupEntry(local_entry.GetCatalog(), func_lookup, schema);
		if (entry && entry->type == CatalogType::TABLE_FUNCTION_ENTRY) {
			auto &tf_entry = entry->Cast<TableFunctionCatalogEntry>();
			bool is_set_returning = false;
			for (auto &func : tf_entry.functions.functions) {
				if (func->return_type == TableFunctionReturnType::SET_RETURNING_FUNCTION) {
					is_set_returning = true;
					break;
				}
			}
			if (!is_set_returning) {
				// TABLE_RETURNING_FUNCTION - blocks pushdown; track alias so correlated
				// refs from nested lateral subqueries are detected
				TrackLocalTable(ref);
				return CatalogPushdownResult::Unknown();
			}
			// SET_RETURNING_FUNCTION: neutral, recurse into args
			// the generic TableRef dispatch records the function so a remote catalog can veto it
			auto result = CatalogPushdownResult::NoCatalogReference();
			for (auto &arg : func_expr.GetArgumentsMutable()) {
				result = Merge(result, RewriteTableFunctionArgument(arg.GetExpressionMutable()));
			}
			return result;
		}
	}
	// we did not find the table function in a local catalog
	if (pushdown_state.remote_catalogs_in_search_path.size() == 1) {
		// if we have a single catalog in the remote search path - assume the function lives there
		auto &remote_catalog = pushdown_state.remote_catalogs_in_search_path[0].get();
		if (remote_catalog.Supports(RemoteCapability::EXECUTE_QUERY_NODE) && remote_catalog.SupportsPushdown(ref)) {
			// "catalog" is remote and we can pushdown this function
			return CatalogPushdownResult::RemoteReference(remote_catalog);
		}
	}
	// we couldn't find the function locally or remotely - skip pushing down
	TrackLocalTable(ref);
	return CatalogPushdownResult::Unknown();
}

CatalogPushdownResult RemotePushdownOptimizer::RewriteTableFunctionArgument(unique_ptr<ParsedExpression> &arg) {
	// folding names the constant after the expression it replaces, but the binder reads an alias on a
	// table function argument as a named parameter - so only an alias the user wrote may survive
	const bool user_aliased = !arg->GetAlias().empty();
	auto result = Rewrite(arg);
	if (!user_aliased) {
		arg->SetAlias(Identifier());
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::Rewrite(TableFunctionRef &ref) {
	// rewrite the table function only
	auto result = RewriteTableFunctionOnly(ref);
	if (result.reference_type == CatalogReferenceType::UNKNOWN_CATALOG_REFERENCE) {
		// don't bother recursing - we can never pushdown
		return result;
	}
	// recurse into the function arguments
	auto &func_expr = ref.function->Cast<FunctionExpression>();
	for (auto &arg : func_expr.GetArgumentsMutable()) {
		result = Merge(result, RewriteTableFunctionArgument(arg.GetExpressionMutable()));
	}
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::Rewrite(JoinRef &ref) {
	const auto cte_refs_before_left = pushdown_state.cte_reference_count;
	auto left_result = Rewrite(ref.left);
	const bool left_refers_to_cte = pushdown_state.cte_reference_count != cte_refs_before_left;

	// the right side of a join can be correlated to the left side - use a child optimizer to track this
	RemotePushdownOptimizer child_optimizer(this);
	const auto cte_refs_before_right = pushdown_state.cte_reference_count;
	auto right_result = child_optimizer.Rewrite(ref.right);
	const bool right_refers_to_cte = pushdown_state.cte_reference_count != cte_refs_before_right;

	auto result = Merge(left_result, right_result);

	// For cross-catalog joins (different remote catalogs, or one remote + one local), each
	// single-remote subtree can be pushed to its own source and the join then runs at the
	// master over the returned results. Only record the candidates here: the enclosing
	// WHERE clause is not visible yet, and pushing without it would ask each source for its
	// entire table. RewriteNode(SelectNode) performs the actual pushdown once it can attach
	// the conjuncts that belong to each side.
	// A side that names a CTE is declined: only the enclosing statement carries the WITH clause,
	// so a fragment built from the side alone would name a CTE the source has never seen.
	if (result.reference_type == CatalogReferenceType::UNKNOWN_CATALOG_REFERENCE) {
		if (left_result.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG &&
		    !left_refers_to_cte) {
			PendingRemoteJoinSide pending {&ref.left, left_result, {}};
			CollectTableAliases(*ref.left, pending.aliases);
			pushdown_state.pending_join_sides.push_back(std::move(pending));
		}
		if (right_result.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG &&
		    !right_refers_to_cte) {
			PendingRemoteJoinSide pending {&ref.right, right_result, {}};
			CollectTableAliases(*ref.right, pending.aliases);
			pushdown_state.pending_join_sides.push_back(std::move(pending));
		}
	}

	// Also analyze the join condition - it may contain subqueries or local macro calls
	// that affect whether the join can be pushed as a whole.
	if (ref.condition) {
		result = Merge(result, Rewrite(ref.condition));
	}
	return result;
}

void RemotePushdownOptimizer::TrackLocalTable(const BaseTableRef &ref) {
	if (!ref.alias.empty()) {
		local_table_names.insert(ref.alias);
	} else {
		local_table_names.insert(ref.Table());
	}
}

void RemotePushdownOptimizer::TrackLocalTable(const TableFunctionRef &ref) {
	if (!ref.alias.empty()) {
		local_table_names.insert(ref.alias);
	} else {
		local_table_names.insert(ref.function->Cast<FunctionExpression>().FunctionName());
	}
}

void RemotePushdownOptimizer::TrackLocalTable(const SubqueryRef &ref) {
	if (!ref.alias.empty()) {
		local_table_names.insert(ref.alias);
	} else {
		local_table_names.insert("unnamed_subquery");
	}
}

bool RemotePushdownOptimizer::RefersToCTE(const Identifier &cte_name, CatalogPushdownResult &result) const {
	auto entry = cte_results.find(cte_name);
	if (entry != cte_results.end()) {
		result = entry->second;
		return true;
	}
	if (parent) {
		return parent->RefersToCTE(cte_name, result);
	}
	return false;
}

CatalogPushdownResult RemotePushdownOptimizer::Rewrite(BaseTableRef &ref) {
	// Resolve the schema-as-catalog ambiguity using the binder's own resolution logic. This keeps the whole
	// (possibly nested) schema path, so a reference like s1.child.t is not mistaken for catalog "s1"
	Identifier catalog_name;
	vector<Identifier> schema_path;
	ResolveQualification(ref.GetQualifiedName(), catalog_name, schema_path);

	// Case 0: check if this is a CTE reference (must have no explicit catalog/schema)
	if (catalog_name.empty() && schema_path.empty()) {
		CatalogPushdownResult pushdown_result;
		if (RefersToCTE(ref.Table(), pushdown_result)) {
			// Record the reference so an enclosing cross-catalog join side that depends on a CTE
			// name is not pushed on its own, without the WITH clause that defines the name.
			pushdown_state.cte_reference_count++;
			if (pushdown_result.reference_type == CatalogReferenceType::UNKNOWN_CATALOG_REFERENCE) {
				// Local/unknown CTE - track as local for correlated subquery detection
				TrackLocalTable(ref);
			}
			return pushdown_result;
		}
	}

	EntryLookupInfo table_lookup(CatalogType::TABLE_ENTRY, QualifiedName(ref.Table()));

	// Case 1: catalog is explicitly specified - check if it's a remote catalog
	if (!catalog_name.empty()) {
		auto result = ResolveRemoteCatalog(catalog_name, RemoteCapability::EXECUTE_QUERY_NODE);
		// verify the table actually exists in the remote catalog - if it does not, fall back
		// to the binder so it can report a proper error message
		if (result.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG &&
		    LookupEntry(result.catalog->GetName(), table_lookup, schema_path)) {
			return result;
		}
		// A local table always blocks pushdown of any query that contains it.
		// Returning UNKNOWN (not NO_CATALOG) ensures Merge(SINGLE_REMOTE, UNKNOWN) = UNKNOWN
		// rather than the otherwise-neutral SINGLE_REMOTE.
		TrackLocalTable(ref);
		return CatalogPushdownResult::Unknown();
	}

	// Case 2: no explicit catalog - lazily populate search path catalogs on first use
	FindRemoteCatalogsInSearchPath();

	if (pushdown_state.remote_catalogs_in_search_path.size() != 1 ||
	    EntryExistsInLocalCatalog(table_lookup, schema_path)) {
		// Same as Case 1: a local table → UNKNOWN to prevent Merge from treating it as neutral.
		TrackLocalTable(ref);
		return CatalogPushdownResult::Unknown();
	}

	// Not found in any local catalog - push to the single remote catalog in the search path,
	// but only if the table actually exists there (otherwise fall back to the binder for a proper error)
	auto &remote_catalog = pushdown_state.remote_catalogs_in_search_path.front().get();
	if (!LookupEntry(remote_catalog.GetName(), table_lookup, schema_path)) {
		TrackLocalTable(ref);
		return CatalogPushdownResult::Unknown();
	}
	return CatalogPushdownResult::RemoteReference(remote_catalog);
}

bool RemotePushdownOptimizer::RefersToLocalTable(const ColumnRefExpression &col_ref) const {
	// figuring out if a column refers to a local table is challenging without knowing all of the columns
	// challenges are:
	// (1) we might have a subquery (e.g. FROM (SELECT ...), (SELECT ...))
	//   - when binding the second subquery we need to know the schema of the first subquery
	// (2) we might have CTEs (e.g. FROM cte, (SELECT ...))
	//   - when binding the subquery we need to know the schema of the CTE
	// (3) we might have struct columns (e.g. FROM tbl, (SELECT struct_col.field)
	//   - we need to correctly deal with this scenario and figure out which struct col this column references
	// this is effectively having to re-implement many components of binding but with some information missing
	if (local_table_names.empty()) {
		return false;
	}
	return true;
}

ExpressionPushdownResult RemotePushdownOptimizer::AnalyzeExpression(const SubqueryExpression &subquery_expr) {
	ExpressionPushdownResult state;
	RemotePushdownOptimizer child_optimizer(this);
	state.result = child_optimizer.Rewrite(*subquery_expr.Subquery()->node);
	return state;
}

CatalogPushdownResult RemotePushdownOptimizer::CheckCatalogQualification(const ParsedExpression &expr,
                                                                         const QualifiedName &name) {
	Identifier catalog_name;
	vector<Identifier> schema_path;
	ResolveQualification(name, catalog_name, schema_path);
	if (catalog_name.empty()) {
		return CatalogPushdownResult::NoCatalogReference();
	}
	// remote: the generic expression dispatch verifies that the catalog supports pushing down this
	// expression. Explicitly local-catalog: block pushdown
	return ResolveRemoteCatalog(catalog_name, RemoteCapability::EXECUTE_QUERY_NODE);
}

ExpressionPushdownResult RemotePushdownOptimizer::AnalyzeExpression(const FunctionExpression &func) {
	ExpressionPushdownResult state;
	state.result = CheckCatalogQualification(func, func.GetQualifiedName());
	// look up the function once - this determines both whether it can be constant-folded and
	// whether it is a macro in a local catalog (which cannot be evaluated remotely)
	EntryLookupInfo function_lookup(CatalogType::SCALAR_FUNCTION_ENTRY, func.GetQualifiedName());
	auto entry = Catalog::GetEntry(binder.context, function_lookup, OnEntryNotFound::RETURN_NULL);
	if (!entry) {
		return state;
	}
	// aggregate-style modifiers cannot be constant-folded
	bool foldable_modifiers = !func.Filter() && !func.Distinct() && !func.ExportState() &&
	                          (!func.OrderBy() || func.OrderBy()->orders.empty());
	switch (entry->type) {
	case CatalogType::MACRO_ENTRY:
		// scalar macros can be folded - the stability of the expansion is checked after binding
		if (foldable_modifiers) {
			state.foldability = ExpressionFoldability::FOLDABLE;
		}
		if (!entry->ParentCatalog().Supports(RemoteCapability::EXECUTE_QUERY_NODE)) {
			// macros in local catalogs cannot be evaluated remotely - if the macro is not folded
			// away, pushdown is blocked
			state.result = CatalogPushdownResult::Unknown();
		}
		break;
	case CatalogType::SCALAR_FUNCTION_ENTRY: {
		// at least one overload must be non-volatile (the selected overload is verified after binding)
		auto &scalar_entry = entry->Cast<ScalarFunctionCatalogEntry>();
		for (auto &overload : scalar_entry.functions.functions) {
			if (foldable_modifiers && overload->GetStability() != FunctionStability::VOLATILE) {
				state.foldability = ExpressionFoldability::FOLDABLE;
				break;
			}
		}
		break;
	}
	default:
		break;
	}
	return state;
}

ExpressionPushdownResult RemotePushdownOptimizer::AnalyzeExpression(const WindowExpression &func) {
	ExpressionPushdownResult state;
	state.result = CheckCatalogQualification(func, func.GetQualifiedName());
	return state;
}

ExpressionPushdownResult RemotePushdownOptimizer::AnalyzeExpression(const TypeExpression &type_expr) {
	ExpressionPushdownResult state;
	state.result = CheckCatalogQualification(type_expr, type_expr.GetQualifiedName());
	return state;
}

ExpressionPushdownResult RemotePushdownOptimizer::AnalyzeExpression(const ColumnRefExpression &col_ref) {
	ExpressionPushdownResult state;
	if (RefersToLocalTable(col_ref)) {
		// column refers to local table - bail
		state.result = CatalogPushdownResult::Unknown();
	}
	return state;
}

RemotePushdownOptimizer::ConstantFoldResult
RemotePushdownOptimizer::TryConstantFold(unique_ptr<ParsedExpression> &expr) {
	// bind a copy of the expression (binding modifies the expression in-place)
	unique_ptr<Expression> bound_expr;
	try {
		auto expr_copy = expr->Copy();
		auto fold_binder = Binder::CreateBinder(binder.context);
		ConstantBinder constant_binder(*fold_binder, binder.context, "remote pushdown");
		bound_expr = constant_binder.Bind(expr_copy);
	} catch (std::exception &) {
		// the expression cannot be bound as a constant (e.g. no matching function overload)
		return ConstantFoldResult::NOT_FOLDABLE;
	}
	if (!bound_expr || bound_expr->HasParameter() || bound_expr->HasSubquery()) {
		return ConstantFoldResult::NOT_FOLDABLE;
	}
	if (bound_expr->IsVolatile()) {
		// volatile functions (random(), ...) must be re-evaluated on every row
		return ConstantFoldResult::NOT_FOLDABLE;
	}
	if (!bound_expr->IsConsistent()) {
		// functions like now() must be re-evaluated (re-folded) when a prepared statement is re-executed
		binder.SetAlwaysRequireRebind();
	}
	Value fold_result;
	if (!ExpressionExecutor::TryEvaluateScalar(binder.context, *bound_expr, fold_result)) {
		// evaluating the expression raises an error (e.g. an out-of-range error)
		return ConstantFoldResult::FOLD_ERROR;
	}
	auto folded = make_uniq<ConstantExpression>(std::move(fold_result));
	// preserve the name DuckDB would generate for the original expression
	folded->SetAlias(expr->GetAlias().empty() ? Identifier(expr->ToString()) : expr->GetAlias());
	folded->SetQueryLocation(expr->GetQueryLocation());
	expr = std::move(folded);
	return ConstantFoldResult::FOLDED;
}

ExpressionPushdownResult RemotePushdownOptimizer::AnalyzeExpression(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::SUBQUERY:
		return AnalyzeExpression(expr.Cast<SubqueryExpression>());
	case ExpressionClass::FUNCTION:
		return AnalyzeExpression(expr.Cast<FunctionExpression>());
	case ExpressionClass::WINDOW:
		return AnalyzeExpression(expr.Cast<WindowExpression>());
	case ExpressionClass::TYPE:
		return AnalyzeExpression(expr.Cast<TypeExpression>());
	case ExpressionClass::COLUMN_REF:
		return AnalyzeExpression(expr.Cast<ColumnRefExpression>());
	case ExpressionClass::CONSTANT:
	case ExpressionClass::CAST:
	case ExpressionClass::COMPARISON:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::CONJUNCTION:
	case ExpressionClass::OPERATOR:
	case ExpressionClass::CASE: {
		// deterministic expression types - foldable when all inputs are foldable
		ExpressionPushdownResult state;
		state.foldability = ExpressionFoldability::FOLDABLE;
		return state;
	}
	default:
		return ExpressionPushdownResult();
	}
}

ExpressionPushdownResult RemotePushdownOptimizer::RewriteExpression(unique_ptr<ParsedExpression> &expr,
                                                                    ExpressionFoldingMode mode) {
	// rewrite the children - foldable subtrees are folded at the last possible moment, so that
	// every maximal foldable subtree is bound and evaluated exactly once
	auto result = CatalogPushdownResult::NoCatalogReference();
	vector<reference<unique_ptr<ParsedExpression>>> foldable_children;
	bool all_children_foldable = true;
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		auto child_state = RewriteExpression(child, ExpressionFoldingMode::FOLD_EXPRESSION);
		if (child_state.foldability == ExpressionFoldability::FOLDABLE) {
			// defer - the subtree is folded when it turns out to be a maximal foldable subtree
			foldable_children.push_back(child);
		} else {
			all_children_foldable = false;
			result = Merge(std::move(result), std::move(child_state.result));
		}
	});
	auto state = AnalyzeExpression(*expr);
	if (mode == ExpressionFoldingMode::FOLD_EXPRESSION && all_children_foldable &&
	    state.foldability == ExpressionFoldability::FOLDABLE) {
		// this expression is itself foldable - defer folding to the parent
		ExpressionPushdownResult folding_deferred;
		folding_deferred.foldability = ExpressionFoldability::FOLDABLE;
		return folding_deferred;
	}
	// this expression is not foldable - fold the (maximal) foldable children now
	for (auto &child : foldable_children) {
		result = Merge(std::move(result), FoldExpression(child.get()));
	}
	result = Merge(std::move(result), std::move(state.result));
	if (result.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		// the expression is fully remote - check if the remote catalog supports pushing it down
		if (!result.catalog->SupportsPushdown(*expr)) {
			result = CatalogPushdownResult::Unknown();
		}
	} else if (result.reference_type == CatalogReferenceType::NO_CATALOG_REFERENCED) {
		// record the expression so a remote catalog can veto pushdown of any expression class
		// (functions, comparisons, operators, star expressions, parameters, etc.) during a later merge
		result.used_expressions.push_back(*expr);
	}
	state.result = std::move(result);
	state.foldability = ExpressionFoldability::NOT_FOLDABLE;
	return state;
}

CatalogPushdownResult RemotePushdownOptimizer::FoldExpression(unique_ptr<ParsedExpression> &expr) {
	if (expr->GetExpressionClass() != ExpressionClass::CONSTANT) {
		// replace the expression with its locally-evaluated result
		switch (TryConstantFold(expr)) {
		case ConstantFoldResult::FOLD_ERROR:
			// evaluating is guaranteed to fail - keep the query local so the user sees DuckDB's error
			return CatalogPushdownResult::Unknown();
		case ConstantFoldResult::NOT_FOLDABLE:
			// binding did not succeed after all - process the expression without re-attempting the fold
			return RewriteExpression(expr, ExpressionFoldingMode::FOLD_CHILDREN_ONLY).result;
		case ConstantFoldResult::FOLDED:
			break;
		}
	}
	// record the constant so a remote catalog can verify that it is supported
	auto result = CatalogPushdownResult::NoCatalogReference();
	result.used_expressions.push_back(*expr);
	return result;
}

CatalogPushdownResult RemotePushdownOptimizer::Rewrite(unique_ptr<ParsedExpression> &expr, ExpressionFoldingMode mode) {
	auto state = RewriteExpression(expr, mode);
	if (state.foldability == ExpressionFoldability::FOLDABLE) {
		// the entire expression is foldable - fold it at the root
		return FoldExpression(expr);
	}
	return state.result;
}

unique_ptr<TableRef> RemotePushdownOptimizer::CreateRemoteFunctionRef(CatalogPushdownResult &result,
                                                                      unique_ptr<QueryNode> node) {
	return result.catalog->RemoteExecute(binder.context, std::move(node));
}

//! Drop the catalog qualifier from a name. A two-part name (e.g. "rpc.t") parses as schema.name, so the
//! catalog being pushed to can sit in either slot
static void StripCatalogFromName(QualifiedName &name, const Identifier &catalog_name) {
	if (name.Catalog() == catalog_name) {
		name.StripCatalog();
	} else if (name.Catalog().empty() && name.Schema() == catalog_name) {
		name = QualifiedName(Identifier(), Identifier(), name.Name());
	}
}

void RemotePushdownOptimizer::StripCatalogName(TableRef &ref, const Identifier &catalog_name) {
	switch (ref.type) {
	case TableReferenceType::BASE_TABLE: {
		auto &base = ref.Cast<BaseTableRef>();
		auto name = base.GetQualifiedName();
		StripCatalogFromName(name, catalog_name);
		base.SetQualifiedName(std::move(name));
		break;
	}
	case TableReferenceType::JOIN: {
		auto &join = ref.Cast<JoinRef>();
		StripCatalogName(*join.left, catalog_name);
		StripCatalogName(*join.right, catalog_name);
		if (join.condition) {
			StripCatalogName(*join.condition, catalog_name);
		}
		break;
	}
	case TableReferenceType::SUBQUERY: {
		auto &sq = ref.Cast<SubqueryRef>();
		StripCatalogName(*sq.subquery->node, catalog_name);
		break;
	}
	case TableReferenceType::TABLE_FUNCTION: {
		auto &tf = ref.Cast<TableFunctionRef>();
		if (tf.function) {
			StripCatalogName(*tf.function, catalog_name);
		}
		break;
	}
	case TableReferenceType::EXPRESSION_LIST: {
		auto &el = ref.Cast<ExpressionListRef>();
		for (auto &row : el.values) {
			for (auto &expr : row) {
				StripCatalogName(*expr, catalog_name);
			}
		}
		break;
	}
	default:
		break;
	}
}

void RemotePushdownOptimizer::StripCatalogName(ParsedExpression &expr, const Identifier &catalog_name) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &col_ref = expr.Cast<ColumnRefExpression>();
		// Strip catalog prefix from qualified column references, normalising to exactly table.col (2 parts).
		// Require at least 3 names: a 2-part ref like "rpc.field" is either table.col or struct-column.field —
		// not catalog-qualified — so stripping would be wrong.
		// For 3-part  catalog.table.col        → table.col   (one level stripped)
		// For 4-part  catalog.schema.table.col → table.col   (catalog + schema stripped)
		if (col_ref.ColumnNames().size() >= 3 && col_ref.ColumnNames()[0] == catalog_name) {
			Identifier table_name = col_ref.ColumnNames()[col_ref.ColumnNames().size() - 2];
			Identifier col_name = col_ref.ColumnNames()[col_ref.ColumnNames().size() - 1];
			col_ref.ColumnNamesMutable() = {std::move(table_name), std::move(col_name)};
		}
		return;
	}
	if (expr.GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subq = expr.Cast<SubqueryExpression>();
		StripCatalogName(*subq.SubqueryMutable()->node, catalog_name);
		if (subq.GetChild()) {
			StripCatalogName(*subq.GetChildMutable(), catalog_name);
		}
		return;
	}
	// Strip catalog prefix from explicitly-qualified function/window/type calls.
	// Also handle 2-part names (schema.func/type) where the schema is actually the remote catalog name
	// (e.g. "rpc.my_func()" parsed as schema="rpc", catalog="").
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &func = expr.Cast<FunctionExpression>();
		if (func.GetQualifiedName().Catalog() == catalog_name) {
			func.SetQualifiedName(
			    QualifiedName(Identifier(), func.GetQualifiedName().Schema(), func.GetQualifiedName().Name()));
		} else if (func.GetQualifiedName().Catalog().empty() && func.GetQualifiedName().Schema() == catalog_name) {
			func.SetQualifiedName(
			    QualifiedName(func.GetQualifiedName().Catalog(), Identifier(), func.GetQualifiedName().Name()));
		}
		// Fall through to EnumerateChildren to also strip catalog refs inside arguments
	} else if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		auto &win = expr.Cast<WindowExpression>();
		if (win.GetQualifiedName().Catalog() == catalog_name) {
			win.SetQualifiedName(
			    QualifiedName(Identifier(), win.GetQualifiedName().Schema(), win.GetQualifiedName().Name()));
		} else if (win.GetQualifiedName().Catalog().empty() && win.GetQualifiedName().Schema() == catalog_name) {
			win.SetQualifiedName(
			    QualifiedName(win.GetQualifiedName().Catalog(), Identifier(), win.GetQualifiedName().Name()));
		}
		// Fall through to EnumerateChildren to strip catalog refs inside partitions/orders/children
	} else if (expr.GetExpressionClass() == ExpressionClass::CAST) {
		// CastExpression stores the cast target as a LogicalType, not an expression child — EnumerateChildren
		// only visits the value being cast. For unbound (user-defined) types we must strip the catalog from the
		// embedded TypeExpression and reconstruct the LogicalType::UNBOUND wrapper.
		auto &cast_expr = expr.Cast<CastExpression>();
		auto &target_type = cast_expr.TargetTypeMutable();
		if (target_type.id() == LogicalTypeId::UNBOUND) {
			auto type_expr = UnboundType::GetTypeExpression(target_type)->Copy();
			StripCatalogName(*type_expr, catalog_name);
			target_type = LogicalType::UNBOUND(std::move(type_expr));
		}
		// Fall through to EnumerateChildren to strip catalog refs inside the cast argument
	} else if (expr.GetExpressionClass() == ExpressionClass::TYPE) {
		// TypeExpression (used as a type argument) may carry catalog/schema qualifiers.
		auto &type_expr = expr.Cast<TypeExpression>();
		if (type_expr.GetCatalog() == catalog_name) {
			type_expr.SetCatalog("");
		} else if (type_expr.GetCatalog().empty() && type_expr.GetSchema() == catalog_name) {
			type_expr.SetSchema("");
		}
		// Fall through to EnumerateChildren to strip catalog refs inside type parameters
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { StripCatalogName(child, catalog_name); });
}

void RemotePushdownOptimizer::StripCatalogName(QueryNode &node, const Identifier &catalog_name) {
	switch (node.type) {
	case QueryNodeType::SELECT_NODE: {
		auto &select = node.Cast<SelectNode>();
		// Strip within CTE definitions so the remote receives catalog-free SQL
		for (auto &cte_pair : select.cte_map.map) {
			if (cte_pair.second->query_node) {
				StripCatalogName(*cte_pair.second->query_node, catalog_name);
			}
			for (auto &key : cte_pair.second->key_targets) {
				StripCatalogName(*key, catalog_name);
			}
		}
		if (select.from_table) {
			StripCatalogName(*select.from_table, catalog_name);
		}
		for (auto &expr : select.select_list) {
			StripCatalogName(*expr, catalog_name);
		}
		if (select.where_clause) {
			StripCatalogName(*select.where_clause, catalog_name);
		}
		for (auto &expr : select.groups.group_expressions) {
			StripCatalogName(*expr, catalog_name);
		}
		if (select.having) {
			StripCatalogName(*select.having, catalog_name);
		}
		if (select.qualify) {
			StripCatalogName(*select.qualify, catalog_name);
		}
		for (auto &modifier : select.modifiers) {
			switch (modifier->type) {
			case ResultModifierType::ORDER_MODIFIER: {
				auto &order_mod = modifier->Cast<OrderModifier>();
				for (auto &order : order_mod.orders) {
					StripCatalogName(*order.expression, catalog_name);
				}
				break;
			}
			case ResultModifierType::LIMIT_MODIFIER: {
				auto &limit_mod = modifier->Cast<LimitModifier>();
				if (limit_mod.limit) {
					StripCatalogName(*limit_mod.limit, catalog_name);
				}
				if (limit_mod.offset) {
					StripCatalogName(*limit_mod.offset, catalog_name);
				}
				break;
			}
			case ResultModifierType::DISTINCT_MODIFIER: {
				auto &distinct_mod = modifier->Cast<DistinctModifier>();
				for (auto &expr : distinct_mod.distinct_on_targets) {
					StripCatalogName(*expr, catalog_name);
				}
				break;
			}
			default:
				break;
			}
		}
		break;
	}
	case QueryNodeType::INSERT_QUERY_NODE: {
		auto &insert = node.Cast<InsertQueryNode>();
		for (auto &cte_pair : insert.cte_map.map) {
			if (cte_pair.second->query_node) {
				StripCatalogName(*cte_pair.second->query_node, catalog_name);
			}
			for (auto &key : cte_pair.second->key_targets) {
				StripCatalogName(*key, catalog_name);
			}
		}
		// Strip from the target table's catalog/schema fields (these are what ToString() serializes)
		StripCatalogFromName(insert.qualified_name, catalog_name);
		if (insert.select_statement) {
			StripCatalogName(*insert.select_statement->node, catalog_name);
		}
		if (insert.on_conflict_info) {
			if (insert.on_conflict_info->condition) {
				StripCatalogName(*insert.on_conflict_info->condition, catalog_name);
			}
			if (insert.on_conflict_info->set_info) {
				if (insert.on_conflict_info->set_info->condition) {
					StripCatalogName(*insert.on_conflict_info->set_info->condition, catalog_name);
				}
				for (auto &expr : insert.on_conflict_info->set_info->expressions) {
					StripCatalogName(*expr, catalog_name);
				}
			}
		}
		for (auto &expr : insert.returning_list) {
			StripCatalogName(*expr, catalog_name);
		}
		break;
	}
	case QueryNodeType::DELETE_QUERY_NODE: {
		auto &del = node.Cast<DeleteQueryNode>();
		for (auto &cte_pair : del.cte_map.map) {
			if (cte_pair.second->query_node) {
				StripCatalogName(*cte_pair.second->query_node, catalog_name);
			}
			for (auto &key : cte_pair.second->key_targets) {
				StripCatalogName(*key, catalog_name);
			}
		}
		if (del.table) {
			StripCatalogName(*del.table, catalog_name);
		}
		if (del.condition) {
			StripCatalogName(*del.condition, catalog_name);
		}
		for (auto &clause : del.using_clauses) {
			StripCatalogName(*clause, catalog_name);
		}
		for (auto &expr : del.returning_list) {
			StripCatalogName(*expr, catalog_name);
		}
		break;
	}
	case QueryNodeType::UPDATE_QUERY_NODE: {
		auto &upd = node.Cast<UpdateQueryNode>();
		for (auto &cte_pair : upd.cte_map.map) {
			if (cte_pair.second->query_node) {
				StripCatalogName(*cte_pair.second->query_node, catalog_name);
			}
			for (auto &key : cte_pair.second->key_targets) {
				StripCatalogName(*key, catalog_name);
			}
		}
		if (upd.table) {
			StripCatalogName(*upd.table, catalog_name);
		}
		if (upd.from_table) {
			StripCatalogName(*upd.from_table, catalog_name);
		}
		if (upd.set_info) {
			if (upd.set_info->condition) {
				StripCatalogName(*upd.set_info->condition, catalog_name);
			}
			for (auto &expr : upd.set_info->expressions) {
				StripCatalogName(*expr, catalog_name);
			}
		}
		for (auto &expr : upd.returning_list) {
			StripCatalogName(*expr, catalog_name);
		}
		break;
	}
	case QueryNodeType::MERGE_QUERY_NODE: {
		auto &merge = node.Cast<MergeQueryNode>();
		for (auto &cte_pair : merge.cte_map.map) {
			if (cte_pair.second->query_node) {
				StripCatalogName(*cte_pair.second->query_node, catalog_name);
			}
			for (auto &key : cte_pair.second->key_targets) {
				StripCatalogName(*key, catalog_name);
			}
		}
		if (merge.target) {
			StripCatalogName(*merge.target, catalog_name);
		}
		if (merge.source) {
			StripCatalogName(*merge.source, catalog_name);
		}
		if (merge.join_condition) {
			StripCatalogName(*merge.join_condition, catalog_name);
		}
		for (auto &entry : merge.actions) {
			for (auto &action : entry.second) {
				if (action->condition) {
					StripCatalogName(*action->condition, catalog_name);
				}
				if (action->update_info) {
					if (action->update_info->condition) {
						StripCatalogName(*action->update_info->condition, catalog_name);
					}
					for (auto &expr : action->update_info->expressions) {
						StripCatalogName(*expr, catalog_name);
					}
				}
				for (auto &expr : action->expressions) {
					StripCatalogName(*expr, catalog_name);
				}
			}
		}
		for (auto &expr : merge.returning_list) {
			StripCatalogName(*expr, catalog_name);
		}
		break;
	}
	case QueryNodeType::SET_OPERATION_NODE: {
		auto &setop = node.Cast<SetOperationNode>();
		for (auto &cte_pair : setop.cte_map.map) {
			if (cte_pair.second->query_node) {
				StripCatalogName(*cte_pair.second->query_node, catalog_name);
			}
			for (auto &key : cte_pair.second->key_targets) {
				StripCatalogName(*key, catalog_name);
			}
		}
		for (auto &child : setop.children) {
			StripCatalogName(*child, catalog_name);
		}
		for (auto &modifier : setop.modifiers) {
			switch (modifier->type) {
			case ResultModifierType::ORDER_MODIFIER: {
				auto &order_mod = modifier->Cast<OrderModifier>();
				for (auto &order : order_mod.orders) {
					StripCatalogName(*order.expression, catalog_name);
				}
				break;
			}
			case ResultModifierType::LIMIT_MODIFIER: {
				auto &limit_mod = modifier->Cast<LimitModifier>();
				if (limit_mod.limit) {
					StripCatalogName(*limit_mod.limit, catalog_name);
				}
				if (limit_mod.offset) {
					StripCatalogName(*limit_mod.offset, catalog_name);
				}
				break;
			}
			case ResultModifierType::DISTINCT_MODIFIER: {
				auto &distinct_mod = modifier->Cast<DistinctModifier>();
				for (auto &expr : distinct_mod.distinct_on_targets) {
					StripCatalogName(*expr, catalog_name);
				}
				break;
			}
			default:
				break;
			}
		}
		break;
	}
	case QueryNodeType::RECURSIVE_CTE_NODE: {
		auto &rec = node.Cast<RecursiveCTENode>();
		for (auto &cte_pair : rec.cte_map.map) {
			if (cte_pair.second->query_node) {
				StripCatalogName(*cte_pair.second->query_node, catalog_name);
			}
			for (auto &key : cte_pair.second->key_targets) {
				StripCatalogName(*key, catalog_name);
			}
		}
		for (auto &key : rec.key_targets) {
			StripCatalogName(*key, catalog_name);
		}
		for (auto &modifier : rec.modifiers) {
			switch (modifier->type) {
			case ResultModifierType::ORDER_MODIFIER: {
				auto &order_mod = modifier->Cast<OrderModifier>();
				for (auto &order : order_mod.orders) {
					StripCatalogName(*order.expression, catalog_name);
				}
				break;
			}
			case ResultModifierType::LIMIT_MODIFIER: {
				auto &limit_mod = modifier->Cast<LimitModifier>();
				if (limit_mod.limit) {
					StripCatalogName(*limit_mod.limit, catalog_name);
				}
				if (limit_mod.offset) {
					StripCatalogName(*limit_mod.offset, catalog_name);
				}
				break;
			}
			case ResultModifierType::DISTINCT_MODIFIER: {
				auto &distinct_mod = modifier->Cast<DistinctModifier>();
				for (auto &expr : distinct_mod.distinct_on_targets) {
					StripCatalogName(*expr, catalog_name);
				}
				break;
			}
			default:
				break;
			}
		}
		if (rec.left) {
			StripCatalogName(*rec.left, catalog_name);
		}
		if (rec.right) {
			StripCatalogName(*rec.right, catalog_name);
		}
		break;
	}
	default:
		break;
	}
}

void RemotePushdownOptimizer::StripCatalogName(CreateInfo &info, const Identifier &catalog_name) {
	if (info.type == CatalogType::SCHEMA_ENTRY) {
		// the name is [catalog, parent schemas..., new schema, <empty name>] - the catalog is only ever
		// the leading component, so the two-part fallback below must not be applied
		if (info.Cast<CreateSchemaInfo>().SchemaCatalog() == catalog_name) {
			info.StripCatalogQualification();
		}
		return;
	}
	auto name = info.GetQualifiedName();
	StripCatalogFromName(name, catalog_name);
	info.SetQualifiedName(std::move(name));
	// a definition body is shipped verbatim, so any catalog qualifier the user wrote in it still has to go
	switch (info.type) {
	case CatalogType::TABLE_ENTRY: {
		auto &table_info = info.Cast<CreateTableInfo>();
		if (table_info.query) {
			StripCatalogName(*table_info.query->node, catalog_name);
		}
		break;
	}
	case CatalogType::VIEW_ENTRY: {
		auto &view_info = info.Cast<CreateViewInfo>();
		if (view_info.query) {
			StripCatalogName(*view_info.query->node, catalog_name);
		}
		break;
	}
	case CatalogType::TYPE_ENTRY: {
		auto &type_info = info.Cast<CreateTypeInfo>();
		if (type_info.query && type_info.query->type == StatementType::SELECT_STATEMENT) {
			StripCatalogName(*type_info.query->Cast<SelectStatement>().node, catalog_name);
		}
		break;
	}
	case CatalogType::INDEX_ENTRY: {
		auto &index_info = info.Cast<CreateIndexInfo>();
		for (auto &expr : index_info.parsed_expressions) {
			StripCatalogName(*expr, catalog_name);
		}
		for (auto &expr : index_info.expressions) {
			StripCatalogName(*expr, catalog_name);
		}
		break;
	}
	case CatalogType::MACRO_ENTRY:
	case CatalogType::TABLE_MACRO_ENTRY: {
		auto &macro_info = info.Cast<CreateMacroInfo>();
		for (auto &macro : macro_info.macros) {
			for (auto &default_param : macro->default_parameters) {
				StripCatalogName(*default_param.second, catalog_name);
			}
			if (macro->type == MacroType::SCALAR_MACRO) {
				StripCatalogName(*macro->Cast<ScalarMacroFunction>().expression, catalog_name);
			} else if (macro->type == MacroType::TABLE_MACRO) {
				StripCatalogName(*macro->Cast<TableMacroFunction>().query_node, catalog_name);
			}
		}
		break;
	}
	default:
		break;
	}
}

void RemotePushdownOptimizer::StripCatalogName(AlterInfo &info, const Identifier &catalog_name) {
	auto name = info.GetQualifiedName();
	StripCatalogFromName(name, catalog_name);
	info.SetQualifiedName(std::move(name));
}

void RemotePushdownOptimizer::StripCatalogName(SQLStatement &statement, const Identifier &catalog_name) {
	switch (statement.type) {
	case StatementType::SELECT_STATEMENT:
		StripCatalogName(*statement.Cast<SelectStatement>().node, catalog_name);
		break;
	case StatementType::INSERT_STATEMENT:
		StripCatalogName(*statement.Cast<InsertStatement>().node, catalog_name);
		break;
	case StatementType::DELETE_STATEMENT:
		StripCatalogName(*statement.Cast<DeleteStatement>().node, catalog_name);
		break;
	case StatementType::UPDATE_STATEMENT:
		StripCatalogName(*statement.Cast<UpdateStatement>().node, catalog_name);
		break;
	case StatementType::MERGE_INTO_STATEMENT:
		StripCatalogName(*statement.Cast<MergeIntoStatement>().node, catalog_name);
		break;
	case StatementType::CREATE_STATEMENT:
		StripCatalogName(*statement.Cast<CreateStatement>().info, catalog_name);
		break;
	case StatementType::DROP_STATEMENT: {
		auto &info = *statement.Cast<DropStatement>().info;
		auto name = info.GetQualifiedName();
		StripCatalogFromName(name, catalog_name);
		info.SetQualifiedName(std::move(name));
		break;
	}
	case StatementType::ALTER_STATEMENT:
		StripCatalogName(*statement.Cast<AlterStatement>().info, catalog_name);
		break;
	default:
		break;
	}
}

unique_ptr<QueryNode> GetNodeFromStatement(SQLStatement &statement) {
	switch (statement.type) {
	case StatementType::SELECT_STATEMENT:
		return std::move(statement.Cast<SelectStatement>().node);
	case StatementType::INSERT_STATEMENT:
		return std::move(statement.Cast<InsertStatement>().node);
	case StatementType::DELETE_STATEMENT:
		return std::move(statement.Cast<DeleteStatement>().node);
	case StatementType::UPDATE_STATEMENT:
		return std::move(statement.Cast<UpdateStatement>().node);
	case StatementType::MERGE_INTO_STATEMENT:
		return std::move(statement.Cast<MergeIntoStatement>().node);
	default:
		return nullptr;
	}
}

unique_ptr<SelectStatement> RemotePushdownOptimizer::WrapRemoteRef(unique_ptr<TableRef> ref) {
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(ref);
	auto select_stmt = make_uniq<SelectStatement>();
	select_stmt->node = std::move(select_node);
	return select_stmt;
}

void RemotePushdownOptimizer::FinishPushdown(unique_ptr<SQLStatement> &statement, CatalogPushdownResult result) {
	if (result.reference_type != CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		return;
	}
	// Strip the catalog name so the remote server doesn't recursively re-push
	StripCatalogName(*statement, result.catalog->GetName());
	auto node = GetNodeFromStatement(*statement);
	if (node) {
		statement = WrapRemoteRef(CreateRemoteFunctionRef(result, std::move(node)));
		return;
	}
	switch (statement->type) {
	case StatementType::CREATE_STATEMENT:
	case StatementType::DROP_STATEMENT:
	case StatementType::ALTER_STATEMENT:
		break;
	default:
		return;
	}
	// a statement that is not built around a query node (DDL) is shipped to the remote as a whole
	statement = WrapRemoteRef(result.catalog->RemoteExecute(binder.context, std::move(statement)));
}

void RemotePushdownOptimizer::FinishPushdown(unique_ptr<QueryNode> &node, CatalogPushdownResult result) {
	if (result.reference_type != CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		return;
	}
	// FIXME: work-around for referencing a CTE in a parent
	// if this query refers to a CTE in the parent we can't push down only this node
	// as the parent CTE is lost. For now we just block all pushdown if the parent has a CTE.
	// we could fix this in a better way by either (1) moving the parent CTE into this node
	// or (2) tracking if we actually refer to a parent CTE
	for (auto opt = this; opt; opt = opt->parent.get()) {
		for (auto &cte_entry : opt->cte_results) {
			auto ref_type = cte_entry.second.reference_type;
			if (ref_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG ||
			    ref_type == CatalogReferenceType::NO_CATALOG_REFERENCED) {
				return;
			}
		}
	}
	StripCatalogName(*node, result.catalog->GetName());
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = CreateRemoteFunctionRef(result, std::move(node));
	node = std::move(select_node);
}

void RemotePushdownOptimizer::FinishPushdown(unique_ptr<TableRef> &ref, CatalogPushdownResult result,
                                            unique_ptr<ParsedExpression> filter) {
	if (result.reference_type != CatalogReferenceType::SINGLE_REMOTE_CATALOG) {
		return;
	}
	// Preserve the alias the query used to reference this table, otherwise column
	// references like "o.customer_id" cannot bind against the replacement ref.
	// With no explicit alias the table name itself acts as the alias.
	Identifier effective_alias = ref->alias;
	if (effective_alias.empty() && ref->type == TableReferenceType::BASE_TABLE) {
		effective_alias = ref->Cast<BaseTableRef>().Table();
	}
	auto column_aliases = ref->column_name_alias;

	// Wrap the table ref in SELECT * FROM <ref> [WHERE <filter>], strip the catalog prefix,
	// then push to remote. Carrying the filter lets the source do the row reduction instead
	// of returning the whole table for the master to filter.
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	select_node->from_table = std::move(ref);
	select_node->where_clause = std::move(filter);
	StripCatalogName(*select_node, result.catalog->GetName());
	ref = CreateRemoteFunctionRef(result, std::move(select_node));
	if (ref) {
		ref->alias = std::move(effective_alias);
		ref->column_name_alias = std::move(column_aliases);
	}
}

} // namespace duckdb
