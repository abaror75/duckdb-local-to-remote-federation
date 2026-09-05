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

//! A test-only storage extension registering the `fake_remote` catalog type. Its whole purpose is
//! to give DuckDB's own test suite a catalog that answers Supports(RemoteCapability::IS_REMOTE),
//! because that is the only thing that turns on the REMOTE_PUSHDOWN optimizer pass.
class FakeRemoteExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
