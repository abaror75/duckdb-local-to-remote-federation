//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/serialization_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/stack.hpp"
#include "duckdb/planner/bound_parameter_map.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/common/enums/compression_type.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/logical_operator_type.hpp"

#include <type_traits>

namespace duckdb {
class ClientContext;
class Catalog;
class DatabaseInstance;
class CompressionInfo;
enum class ExpressionType : uint8_t;

// The data members live in this base so that the move specifications of SerializationData can be computed from the
// members themselves rather than from a hand-maintained list of member types: a member added here is accounted for
// automatically, and the specifications can never silently claim more than the members provide.
struct SerializationDataMembers {
	struct CustomData {
		virtual ~CustomData() = default;
	};

	stack<reference<ClientContext>> contexts;
	stack<reference<DatabaseInstance>> databases;
	stack<reference<Catalog>> catalogs;
	stack<idx_t> enums;
	stack<reference<bound_parameter_map_t>> parameter_data;
	stack<const_reference<LogicalType>> types;
	stack<const_reference<CompressionInfo>> compression_infos;
	duckdb::unordered_map<std::string, duckdb::stack<duckdb::reference<CustomData>>> customs;
};

struct SerializationData : SerializationDataMembers {
	// stack is backed by std::deque, whose move constructor is not nothrow in every standard library, so the
	// guarantee cannot be stated unconditionally - it is computed from the members instead. Move construction and
	// move assignment are computed separately because they do not necessarily agree (with libstdc++, only the
	// latter is nothrow).
	static constexpr bool NOTHROW_MOVE_CTOR = std::is_nothrow_move_constructible<SerializationDataMembers>::value;
	static constexpr bool NOTHROW_MOVE_ASSIGN = std::is_nothrow_move_assignable<SerializationDataMembers>::value;

	// Out-of-line so the special members touching stack<const_reference<CompressionInfo>> are only instantiated where
	// the type is complete (serialization_data.cpp); other TUs may reach this header with CompressionInfo
	// forward-declared. The specifications above only need the members' declarations, not their definitions, so they
	// are safe to evaluate here.
	SerializationData();
	SerializationData(const SerializationData &);
	SerializationData(SerializationData &&) noexcept(NOTHROW_MOVE_CTOR);
	SerializationData &operator=(const SerializationData &);
	SerializationData &operator=(SerializationData &&) noexcept(NOTHROW_MOVE_ASSIGN);
	~SerializationData();

	template <class T>
	void Set(T entry) = delete;

	template <class T>
	T Get() = delete;

	template <class T>
	optional_ptr<T> TryGet() = delete;

	template <class T>
	void Unset() = delete;

	template <class T>
	inline void AssertNotEmpty(const stack<T> &e) {
		if (e.empty()) {
			throw InternalException("SerializationData - unexpected empty stack");
		}
	}

	template <typename T>
	typename std::enable_if<std::is_base_of<CustomData, T>::value, T &>::type GetCustom() const {
		std::string type = T::GetType();
		auto iter = customs.find(type);
		if (iter == customs.end()) {
			throw duckdb::InternalException("SeserializationData - no stack for %s", type);
		}
		auto &stack = iter->second;
		if (stack.empty()) {
			throw duckdb::InternalException("SerializationData - unexpected empty stack for %s", type);
		}
		// the stack is keyed on T::GetType(), so it can only hold a T
		return static_cast<T &>(stack.top().get());
	}

	template <typename T>
	typename std::enable_if<std::is_base_of<CustomData, T>::value, void>::type SetCustom(T &data) {
		std::string type = T::GetType();
		auto iter = customs.find(type);
		if (iter == customs.end()) {
			iter = customs.emplace(type, duckdb::stack<duckdb::reference<CustomData>> {}).first;
		}
		auto &stack = iter->second;
		stack.push(data);
	}
};

template <>
inline void SerializationData::Set(ExpressionType type) {
	enums.push(idx_t(type));
}

template <>
inline ExpressionType SerializationData::Get() {
	AssertNotEmpty(enums);
	return ExpressionType(enums.top());
}

template <>
inline void SerializationData::Unset<ExpressionType>() {
	AssertNotEmpty(enums);
	enums.pop();
}

template <>
inline void SerializationData::Set(LogicalOperatorType type) {
	enums.push(idx_t(type));
}

template <>
inline LogicalOperatorType SerializationData::Get() {
	AssertNotEmpty(enums);
	return LogicalOperatorType(enums.top());
}

template <>
inline void SerializationData::Unset<LogicalOperatorType>() {
	AssertNotEmpty(enums);
	enums.pop();
}

template <>
inline void SerializationData::Set(CompressionType type) {
	enums.push(idx_t(type));
}

template <>
inline CompressionType SerializationData::Get() {
	AssertNotEmpty(enums);
	return CompressionType(enums.top());
}

template <>
inline void SerializationData::Unset<CompressionType>() {
	AssertNotEmpty(enums);
	enums.pop();
}

template <>
inline void SerializationData::Set(CatalogType type) {
	enums.push(idx_t(type));
}

template <>
inline CatalogType SerializationData::Get() {
	AssertNotEmpty(enums);
	return CatalogType(enums.top());
}

template <>
inline void SerializationData::Unset<CatalogType>() {
	AssertNotEmpty(enums);
	enums.pop();
}

template <>
inline void SerializationData::Set(ClientContext &context) {
	contexts.emplace(context);
}

template <>
inline ClientContext &SerializationData::Get() {
	AssertNotEmpty(contexts);
	return contexts.top();
}

template <>
inline void SerializationData::Unset<ClientContext>() {
	AssertNotEmpty(contexts);
	contexts.pop();
}

template <>
inline void SerializationData::Set(Catalog &catalog) {
	catalogs.emplace(catalog);
}

template <>
inline Catalog &SerializationData::Get() {
	AssertNotEmpty(catalogs);
	return catalogs.top();
}

template <>
inline optional_ptr<Catalog> SerializationData::TryGet() {
	return catalogs.empty() ? nullptr : &catalogs.top().get();
}

template <>
inline void SerializationData::Unset<Catalog>() {
	AssertNotEmpty(catalogs);
	catalogs.pop();
}
template <>
inline void SerializationData::Set(DatabaseInstance &db) {
	databases.emplace(db);
}

template <>
inline DatabaseInstance &SerializationData::Get() {
	AssertNotEmpty(databases);
	return databases.top();
}

template <>
inline void SerializationData::Unset<DatabaseInstance>() {
	AssertNotEmpty(databases);
	databases.pop();
}

template <>
inline void SerializationData::Set(bound_parameter_map_t &context) {
	parameter_data.emplace(context);
}

template <>
inline bound_parameter_map_t &SerializationData::Get() {
	AssertNotEmpty(parameter_data);
	return parameter_data.top();
}

template <>
inline void SerializationData::Unset<bound_parameter_map_t>() {
	AssertNotEmpty(parameter_data);
	parameter_data.pop();
}

template <>
inline void SerializationData::Set(LogicalType &type) {
	types.emplace(type);
}

template <>
inline void SerializationData::Unset<LogicalType>() {
	AssertNotEmpty(types);
	types.pop();
}

template <>
inline void SerializationData::Set(const LogicalType &type) {
	types.emplace(type);
}

template <>
inline const LogicalType &SerializationData::Get() {
	AssertNotEmpty(types);
	return types.top();
}

template <>
inline void SerializationData::Unset<const LogicalType>() {
	AssertNotEmpty(types);
	types.pop();
}

template <>
inline void SerializationData::Set(const CompressionInfo &compression_info) {
	compression_infos.emplace(compression_info);
}

template <>
inline const CompressionInfo &SerializationData::Get() {
	AssertNotEmpty(compression_infos);
	return compression_infos.top();
}

template <>
inline void SerializationData::Unset<const CompressionInfo>() {
	AssertNotEmpty(compression_infos);
	compression_infos.pop();
}

} // namespace duckdb
