//===----------------------------------------------------------------------===//
//                         DuckDB
//
// fake_remote_extension.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! A test-only extension providing the `fake_remote` catalog type. It exists so the test suite has
//! a catalog that reports itself as remote, which is what any pushdown pass waits for.
class FakeRemoteExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
