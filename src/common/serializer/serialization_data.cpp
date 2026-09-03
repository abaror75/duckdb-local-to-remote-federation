#include "duckdb/common/serializer/serialization_data.hpp"

// Full definition of CompressionInfo, so the special members touching the stack<const_reference<CompressionInfo>>
// member are instantiated here (where the type is complete) rather than in every TU that reaches the header.
#include "duckdb/function/compression_info.hpp"

namespace duckdb {

SerializationData::SerializationData() = default;
SerializationData::SerializationData(const SerializationData &) = default;
SerializationData::SerializationData(SerializationData &&) noexcept(NOTHROW_MOVE_CTOR) = default;
SerializationData &SerializationData::operator=(const SerializationData &) = default;
SerializationData &SerializationData::operator=(SerializationData &&) noexcept(NOTHROW_MOVE_ASSIGN) = default;

SerializationData::~SerializationData() = default;

// The specifications above are computed from SerializationDataMembers, so they only describe SerializationData as
// long as it adds no data members of its own. A member added to the derived struct instead of the base would not be
// accounted for, and a specification that is too strong terminates at runtime rather than failing to compile.
static_assert(sizeof(SerializationData) == sizeof(SerializationDataMembers),
              "add data members to SerializationDataMembers, not to SerializationData, so that the move "
              "specifications keep accounting for them");

} // namespace duckdb
