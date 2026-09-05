#include "fake_remote_extension.hpp"

#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/expressionlistref.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

namespace duckdb {

namespace {

//! The result the fake source hands back: a one-row, one-column table holding the SQL text the
//! optimizer deparsed. That text is the interesting output of the pushdown pass, so returning it
//! as data means a .test file can assert on it with a plain query and no new introspection.
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

//! Storage is a plain DuckCatalog: tables live locally and are created by ordinary DDL from the
//! test file. Only the remote capabilities and RemoteExecute are new, which is the whole point --
//! the pushdown pass needs a catalog that says it is remote, not a catalog that is remote.
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
			// EXECUTE_STATEMENT is deliberately absent, so DDL and DML stay local and the tables
			// the optimizer looks up during pushdown genuinely exist.
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

void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	auto storage = make_shared_ptr<StorageExtension>();
	storage->attach = FakeRemoteAttach;
	storage->create_transaction_manager = FakeRemoteTransactionManager;
	StorageExtension::Register(config, "fake_remote", std::move(storage));
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
