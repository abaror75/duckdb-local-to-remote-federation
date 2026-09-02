//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/remote_pushdown_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/tokens.hpp"

namespace duckdb {
class Binder;
class Catalog;
class CatalogEntry;
class ExpressionListRef;
class FunctionExpression;
class JoinRef;
class SubqueryRef;
class TableFunctionRef;
class TableRef;
class QueryNode;
class RecursiveCTENode;
class SetOperationNode;
class InsertQueryNode;
class DeleteQueryNode;
class UpdateQueryNode;
class MergeQueryNode;
class SelectStatement;
class CatalogEntry;
struct EntryLookupInfo;
enum class RemoteCapability : uint8_t;
struct AlterInfo;
struct CreateInfo;
struct CreateTableInfo;
struct DropInfo;

enum class CatalogReferenceType { NO_CATALOG_REFERENCED, SINGLE_REMOTE_CATALOG, UNKNOWN_CATALOG_REFERENCE };

struct CatalogPushdownResult {
	explicit CatalogPushdownResult(
	    CatalogReferenceType reference_type = CatalogReferenceType::UNKNOWN_CATALOG_REFERENCE);
	static CatalogPushdownResult Unknown();
	static CatalogPushdownResult NoCatalogReference();
	static CatalogPushdownResult RemoteReference(Catalog &catalog);

	CatalogReferenceType reference_type = CatalogReferenceType::UNKNOWN_CATALOG_REFERENCE;
	optional_ptr<Catalog> catalog;
	vector<const_reference<ParsedExpression>> used_expressions;
	vector<const_reference<TableRef>> used_table_constructs;
	vector<const_reference<QueryNode>> used_nodes;
};

//! Whether an expression (tree) can be constant-folded
enum class ExpressionFoldability { FOLDABLE, NOT_FOLDABLE };

//! Whether an expression itself may be constant-folded
enum class ExpressionFoldingMode {
	//! The expression (or any subtree within) can be folded
	FOLD_EXPRESSION,
	//! Only subtrees within the expression can be folded, the expression itself must be kept -
	//! used for positional contexts (ORDER BY / GROUP BY / DISTINCT ON, where a folded integer
	//! literal would become a positional reference) and after a failed folding attempt
	FOLD_CHILDREN_ONLY
};

//! The result of analyzing or rewriting a single expression
struct ExpressionPushdownResult {
	//! The catalog analysis result
	CatalogPushdownResult result {CatalogReferenceType::NO_CATALOG_REFERENCED};
	ExpressionFoldability foldability = ExpressionFoldability::NOT_FOLDABLE;
};

//! A join side that resolves to a single remote catalog while the join as a whole does not.
//! Recorded while rewriting the FROM clause and pushed later from RewriteNode(SelectNode),
//! where the WHERE clause is available and its conjuncts can travel with the fragment.
struct PendingRemoteJoinSide {
	//! The slot inside the owning JoinRef to overwrite with the remote scan
	unique_ptr<TableRef> *ref_slot;
	//! Which remote catalog this side belongs to
	CatalogPushdownResult result;
	//! Table aliases visible under this side, used to decide which conjuncts may be pushed
	identifier_set_t aliases;
};

struct RemotePushdownState {
	bool search_path_initialized = false;
	vector<reference<Catalog>> remote_catalogs_in_search_path;
	vector<CatalogSearchEntry> local_catalogs_in_search_path;
	//! Cross-catalog join sides awaiting pushdown, shared across parent/child optimizers so a
	//! nested join on the right-hand side (analyzed by a child optimizer) is not lost
	vector<PendingRemoteJoinSide> pending_join_sides;
	//! Names the alias of each grouped multi-table fragment (__fed_j0, __fed_j1, ...)
	idx_t grouped_fragment_counter = 0;
};

class RemotePushdownOptimizer {
public:
	explicit RemotePushdownOptimizer(Binder &binder);
	explicit RemotePushdownOptimizer(optional_ptr<RemotePushdownOptimizer> parent);

	void Rewrite(unique_ptr<SQLStatement> &statement);

private:
	void FindRemoteCatalogsInSearchPath();
	CatalogPushdownResult Rewrite(QueryNode &node);
	//! The per-type query node handlers are deliberately NOT overloads of Rewrite: calling
	//! Rewrite with a statically-typed node (e.g. *InsertStatement::node, which is an
	//! InsertQueryNode) must dispatch through Rewrite(QueryNode &) so the node-level checks
	//! (CTE handling, catalog support verification) are applied
	CatalogPushdownResult RewriteNode(SelectNode &node);
	CatalogPushdownResult RewriteNode(SetOperationNode &node);
	CatalogPushdownResult RewriteNode(InsertQueryNode &node);
	CatalogPushdownResult RewriteNode(DeleteQueryNode &node);
	CatalogPushdownResult RewriteNode(UpdateQueryNode &node);
	CatalogPushdownResult RewriteNode(MergeQueryNode &node);
	//! Whether the target entry of a DDL statement already exists (DROP/ALTER) or is being created (CREATE).
	//! The two resolve differently when the statement's name carries no explicit catalog qualifier
	enum class DDLTarget { NEW_ENTRY, EXISTING_ENTRY };
	//! The per-statement DDL handlers. Like the query node handlers above these are deliberately not
	//! overloads of Rewrite - they are only ever reached from Rewrite(unique_ptr<SQLStatement> &)
	CatalogPushdownResult RewriteStatement(CreateStatement &statement);
	CatalogPushdownResult RewriteStatement(DropStatement &statement);
	CatalogPushdownResult RewriteStatement(AlterStatement &statement);
	//! Analyze the CTAS query of a CREATE statement. "target" is where the table itself is created - when
	//! the query is fully remote but the table is not, the query alone is pushed down. Nothing else a
	//! CREATE carries is analyzed: the pushdown decision is made by the target catalog alone
	CatalogPushdownResult RewriteCreateInfo(CreateInfo &info, const CatalogPushdownResult &target);
	//! Resolve the catalog that the target of a DDL statement lives in
	CatalogPushdownResult ResolveDDLTarget(const QualifiedName &name, DDLTarget target, CatalogType entry_type);
	//! Resolve an explicitly named catalog to a remote reference, or Unknown if it is local / missing
	CatalogPushdownResult ResolveRemoteCatalog(const Identifier &catalog_name, RemoteCapability capability);
	//! Give the resolved catalog the chance to veto pushing down this statement as a whole
	CatalogPushdownResult VerifyStatementSupport(const SQLStatement &statement, CatalogPushdownResult target);
	//! Resolve a (possibly nested) qualified name into the catalog it lives in and its schema path. This runs the
	//! same catalog/schema ambiguity resolution the binder does, and - unlike Catalog()/Schema() - keeps every
	//! level of a nested schema path. The catalog is empty when the name is not catalog-qualified
	void ResolveQualification(const QualifiedName &name, Identifier &catalog_name, vector<Identifier> &schema_path);
	//! Look an entry up in a catalog, defaulting to the main schema when the name carries no schema
	optional_ptr<CatalogEntry> LookupEntry(const Identifier &catalog_name, const EntryLookupInfo &lookup,
	                                       const vector<Identifier> &schema_path);
	//! Whether any non-remote catalog in the search path holds this entry
	bool EntryExistsInLocalCatalog(const EntryLookupInfo &lookup, const vector<Identifier> &schema_path);
	CatalogPushdownResult Rewrite(unique_ptr<TableRef> &ref);
	CatalogPushdownResult Rewrite(ExpressionListRef &ref);
	CatalogPushdownResult RewriteNode(RecursiveCTENode &node);
	CatalogPushdownResult Rewrite(JoinRef &ref);
	CatalogPushdownResult Rewrite(SubqueryRef &ref);
	CatalogPushdownResult Rewrite(TableFunctionRef &ref);
	CatalogPushdownResult Rewrite(BaseTableRef &ref);

	enum class ConstantFoldResult {
		//! The expression is not a foldable constant expression (contains columns, is volatile, ...)
		NOT_FOLDABLE,
		//! The expression was replaced with its locally-evaluated constant result
		FOLDED,
		//! The expression is constant but evaluating it raises an error - the query must be
		//! executed locally so the user sees DuckDB's error message
		FOLD_ERROR
	};
	//! Rewrite an expression, constant-folding maximal foldable subtrees
	CatalogPushdownResult Rewrite(unique_ptr<ParsedExpression> &expr,
	                              ExpressionFoldingMode mode = ExpressionFoldingMode::FOLD_EXPRESSION);
	//! Rewrite an expression, deferring the folding of foldable subtrees to the parent (or the root)
	ExpressionPushdownResult RewriteExpression(unique_ptr<ParsedExpression> &expr, ExpressionFoldingMode mode);
	//! Fold a maximal foldable subtree and record the resulting constant
	CatalogPushdownResult FoldExpression(unique_ptr<ParsedExpression> &expr);
	//! Per-expression-class catalog analysis (catalog qualification, subqueries, local tables),
	//! which also determines whether the expression can be constant-folded
	ExpressionPushdownResult AnalyzeExpression(const ParsedExpression &expr);
	ExpressionPushdownResult AnalyzeExpression(const SubqueryExpression &expr);
	ExpressionPushdownResult AnalyzeExpression(const FunctionExpression &expr);
	ExpressionPushdownResult AnalyzeExpression(const WindowExpression &expr);
	ExpressionPushdownResult AnalyzeExpression(const TypeExpression &expr);
	ExpressionPushdownResult AnalyzeExpression(const ColumnRefExpression &expr);
	//! Bind and evaluate an expression locally, replacing it with the resulting constant
	ConstantFoldResult TryConstantFold(unique_ptr<ParsedExpression> &expr);
	//! Rewrite a table function argument, keeping it positional if it was not written as name => value
	CatalogPushdownResult RewriteTableFunctionArgument(unique_ptr<ParsedExpression> &arg);

	CatalogPushdownResult CheckCatalogQualification(const ParsedExpression &expr, const QualifiedName &name);
	CatalogPushdownResult RewriteTableFunctionOnly(TableFunctionRef &ref);

	//! Records a BaseTableRef's name, alias and columns as local for correlated subquery detection
	void TrackLocalTable(const TableRef &ref);
	void TrackLocalTable(const BaseTableRef &ref);
	void TrackLocalTable(const TableFunctionRef &ref);
	void TrackLocalTable(const SubqueryRef &ref);

	void FinishPushdown(unique_ptr<SQLStatement> &statement, CatalogPushdownResult result);
	void FinishPushdown(unique_ptr<QueryNode> &node, CatalogPushdownResult result);
	//! Push a single TableRef to its remote catalog (used for cross-catalog joins).
	//! When filter is set it becomes the WHERE clause of the pushed "SELECT * FROM <ref>",
	//! so the remote side does the row reduction instead of shipping the whole table.
	void FinishPushdown(unique_ptr<TableRef> &ref, CatalogPushdownResult result,
	                    unique_ptr<ParsedExpression> filter = nullptr);

	//! Collect the table aliases (or table names when unaliased) visible under a TableRef subtree.
	static void CollectTableAliases(const TableRef &ref, identifier_set_t &aliases);
	//! True when every column reference in expr is qualified by an alias in aliases, there is at
	//! least one such reference, and the expression is safe to evaluate remotely.
	static bool CanPushConjunctTo(const ParsedExpression &expr, const identifier_set_t &aliases);
	//! Split a conjunctive predicate into its AND-separated conjuncts (no ownership transfer).
	static void CollectConjuncts(ParsedExpression &expr, vector<reference<ParsedExpression>> &conjuncts);
	//! Build the AND of every conjunct in where_clause that can be pushed to aliases, or nullptr.
	static unique_ptr<ParsedExpression> BuildPushableFilter(optional_ptr<ParsedExpression> where_clause,
	                                                        const identifier_set_t &aliases);
	//! Push each single-remote side of a cross-catalog join, carrying the WHERE conjuncts that
	//! only reference that side. Called from RewriteNode(SelectNode) where the WHERE is visible.
	//! Only pending entries at or after pending_base are processed.
	void PushCrossCatalogJoinSides(SelectNode &node, idx_t pending_base);
	//! Push a remote subtree that binds several tables as ONE fragment, so the join between
	//! them executes at the source. Returns false when the grouped form cannot be built (an
	//! unresolvable column list, a non-base-table leaf, ...) and the caller should fall back
	//! to pushing the leaves individually.
	bool PushRemoteSubtreeGrouped(unique_ptr<TableRef> &ref, CatalogPushdownResult result, SelectNode &node);
	//! Collect the base tables under a subtree together with the alias each is bound to.
	//! Returns false if the subtree contains anything other than base tables and inner joins.
	static bool CollectGroupableBaseTables(TableRef &ref,
	                                       vector<std::pair<Identifier, reference<BaseTableRef>>> &tables);
	//! Rewrite every "alias.column" in the enclosing node to "fragment_alias.alias__column"
	//! so it binds against the flattened, prefix-projected remote result.
	static void RequalifyColumnRefs(SelectNode &node, const identifier_set_t &pushed_aliases,
	                                const Identifier &fragment_alias);
	static void RequalifyColumnRefsInExpression(unique_ptr<ParsedExpression> &expr,
	                                            const identifier_set_t &pushed_aliases,
	                                            const Identifier &fragment_alias);
	static void RequalifyColumnRefsInTableRef(TableRef &ref, const identifier_set_t &pushed_aliases,
	                                          const Identifier &fragment_alias);
	//! Push every base table under a remote subtree individually, so each keeps its own alias.
	void PushRemoteSubtreeTables(unique_ptr<TableRef> &ref, CatalogPushdownResult result, SelectNode &node);
	//! Wrap a table ref that produces a remote statement's result into "SELECT * FROM <ref>"
	static unique_ptr<SelectStatement> WrapRemoteRef(unique_ptr<TableRef> ref);

	static CatalogPushdownResult Merge(CatalogPushdownResult a, CatalogPushdownResult b);
	unique_ptr<TableRef> CreateRemoteFunctionRef(CatalogPushdownResult &result, unique_ptr<QueryNode> node);
	static void StripCatalogName(SQLStatement &statement, const Identifier &catalog_name);
	static void StripCatalogName(QueryNode &node, const Identifier &catalog_name);
	static void StripCatalogName(TableRef &ref, const Identifier &catalog_name);
	static void StripCatalogName(CreateInfo &info, const Identifier &catalog_name);
	static void StripCatalogName(AlterInfo &info, const Identifier &catalog_name);
	//! Strip catalog prefix from expression column refs. When strip_subquery_bodies=false, leaves subquery
	//! bodies untouched (used for partial pushdown where inner subqueries are not being pushed).
	static void StripCatalogName(ParsedExpression &expr, const Identifier &catalog_name);
	bool RefersToLocalTable(const ColumnRefExpression &col_ref) const;

	bool RefersToCTE(const Identifier &cte_name, CatalogPushdownResult &result) const;

private:
	Binder &binder;
	optional_ptr<RemotePushdownOptimizer> parent;
	unique_ptr<RemotePushdownState> owned_pushdown_state;
	RemotePushdownState &pushdown_state;
	//! Names/aliases of non-remote tables seen in the current FROM scope, used to detect correlated subqueries
	identifier_set_t local_table_names;
	//! CTE name to catalog pushdown result, populated as CTEs are analyzed (inner scopes restore on exit)
	identifier_map_t<CatalogPushdownResult> cte_results;
};
} // namespace duckdb
