#include "fake_remote_extension.hpp"

#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/optimizer/remote_pushdown_optimizer.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

namespace duckdb {

namespace {

//! What the fake source answers with: one row, one column, holding the SQL it was sent. Returning
//! the SQL as data lets a .test file check it with an ordinary query.
unique_ptr<TableRef> SqlAsTable(const string &sql) {
	auto ref = make_uniq<ExpressionListRef>();
	ref->expected_names = {Identifier("fragment_sql")};
	ref->expected_types = {LogicalType::VARCHAR};
	vector<unique_ptr<ParsedExpression>> row;
	row.push_back(make_uniq<ConstantExpression>(Value(sql)));
	ref->values.push_back(std::move(row));
	ref->alias = Identifier("fake_remote_fragment");
	return std::move(ref);
}

//! An ordinary DuckCatalog underneath, so the tables really exist and plain CREATE TABLE makes
//! them. All that is different is that it claims to be remote and answers RemoteExecute. Pushdown
//! only needs a catalog that says it is remote, not one that is.
class FakeRemoteCatalog : public DuckCatalog {
public:
	explicit FakeRemoteCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}

public:
	bool Supports(RemoteCapability capability) const override {
		switch (capability) {
		case RemoteCapability::IS_REMOTE:
		case RemoteCapability::EXECUTE_QUERY_NODE:
			return true;
		default:
			// EXECUTE_STATEMENT is left out on purpose, so CREATE and INSERT stay local and the
			// tables really are there to be found.
			return false;
		}
	}

	unique_ptr<TableRef> RemoteExecute(ClientContext &context, unique_ptr<QueryNode> node) override {
		return SqlAsTable(node->ToString());
	}
};

unique_ptr<Catalog> FakeRemoteAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                     AttachedDatabase &db, const string &name, AttachInfo &info,
                                     AttachOptions &attach_options) {
	info.path = ":memory:";
	auto catalog = make_uniq<FakeRemoteCatalog>(db);
	catalog->Initialize(false);
	return std::move(catalog);
}

unique_ptr<TransactionManager> FakeRemoteTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                            AttachedDatabase &db, Catalog &catalog) {
	return make_uniq<DuckTransactionManager>(db);
}

//===--------------------------------------------------------------------===//
// A pushdown strategy living in an extension
//
// This is the point of the branch: the walk stays in DuckDB, and the decision of what to do with a
// cross-catalog join is made out here, through RemotePushdownHandler. JoinAnalyzed fires once both
// sides have been classified, which is where a strategy gets to act.
//
// The strategy is the simplest one that needs more than a single table: when a join has one side
// wholly on a remote catalog and the other not, send that side as one fragment. Doing it needs four
// things from the optimizer - the catalog the side resolved to, the name resolution it already did,
// the catalog-name stripping, and the scan that stands in for the fragment.
//===--------------------------------------------------------------------===//

class FakeRemoteHandler : public RemotePushdownHandler {
public:
	void JoinAnalyzed(RemotePushdownOptimizer &optimizer, JoinRef &ref, const JoinSideAnalysis &analysis) override {
		// One side remote, the other not. A join wholly on one catalog is DuckDB's own case and it
		// has already dealt with it; a join with neither side remote is nothing to do with us.
		auto left_remote = analysis.left.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG;
		auto right_remote = analysis.right.reference_type == CatalogReferenceType::SINGLE_REMOTE_CATALOG;
		if (left_remote == right_remote) {
			return;
		}
		auto &slot = left_remote ? ref.left : ref.right;
		if (!slot || slot->type != TableReferenceType::BASE_TABLE) {
			return;
		}
		// A copy, because the analysis is handed out read-only and CreateRemoteFunctionRef takes it mutably
		auto result = left_remote ? analysis.left : analysis.right;
		SendAsFragment(optimizer, slot, result);
	}

private:
	//! "SELECT * FROM <table>", stripped of the catalog name and handed back as a scan.
	void SendAsFragment(RemotePushdownOptimizer &optimizer, unique_ptr<TableRef> &slot, CatalogPushdownResult &result) {
		auto &table = slot->Cast<BaseTableRef>();
		auto fragment = make_uniq<SelectNode>();
		fragment->select_list.push_back(make_uniq<StarExpression>());
		auto source = make_uniq<BaseTableRef>();
		source->SetQualifiedName(table.GetQualifiedName());
		source->alias = table.alias;
		fragment->from_table = std::move(source);

		RemotePushdownOptimizer::StripCatalogName(*fragment, result.catalog->GetName());
		auto alias = table.alias.empty() ? table.Table() : table.alias;
		auto pushed = optimizer.CreateRemoteFunctionRef(result, std::move(fragment));
		if (pushed) {
			pushed->alias = alias;
			slot = std::move(pushed);
		}
	}
};

void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	auto storage = make_shared_ptr<StorageExtension>();
	storage->attach = FakeRemoteAttach;
	storage->create_transaction_manager = FakeRemoteTransactionManager;
	StorageExtension::Register(config, "fake_remote", std::move(storage));

	// Installed unconditionally: it only acts on a join whose two sides disagree, which is the case
	// DuckDB's own pass leaves alone, so it cannot change what the other tests here expect.
	config.create_remote_pushdown_handler = []() -> unique_ptr<RemotePushdownHandler> {
		return make_uniq<FakeRemoteHandler>();
	};
}

} // namespace

void FakeRemoteExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string FakeRemoteExtension::Name() {
	return "fake_remote";
}

std::string FakeRemoteExtension::Version() const {
	return "";
}

} // namespace duckdb
