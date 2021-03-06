/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef VC4C_CONCEPTS_H
#define VC4C_CONCEPTS_H

#include "InstructionWalker.h"
#include "Module.h"
#include "asm/ALUInstruction.h"
#include "llvm/Token.h"

#include <type_traits>

namespace vc4c
{

	template<typename T>
	struct assert_copyable
	{
		constexpr static bool value = std::is_copy_constructible<T>::value && std::is_copy_assignable<T>::value;
	};

	template<typename T>
	struct assert_moveable
	{
		constexpr static bool value = std::is_move_constructible<T>::value && std::is_move_assignable<T>::value;
	};

	template<typename T>
	struct assert_assignable
	{
		constexpr static bool value = assert_copyable<T>::value && assert_moveable<T>::value;
	};

	template<typename T>
	struct assert_not_copyable
	{
		constexpr static bool value = !std::is_copy_constructible<T>::value && !std::is_copy_assignable<T>::value;
	};

	template<typename T>
	struct assert_not_moveable
	{
		constexpr static bool value = !std::is_move_constructible<T>::value && !std::is_move_assignable<T>::value;
	};

	template<typename T>
	struct assert_not_assignable
	{
		constexpr static bool value = assert_not_copyable<T>::value && assert_not_moveable<T>::value;
	};

	/*
	 * DataType
	 *
	 * (most of this concepts are required by Value)
	 */
	static_assert(std::is_default_constructible<DataType>::value, "DataType is not default constructible!");
	static_assert(assert_assignable<DataType>::value, "DataType is not assignable!");
	static_assert(std::is_destructible<DataType>::value, "DataType is not destructible!");

	/*
	 * Value types
	 *
	 * (most of this concepts are required by Value)
	 */
	static_assert(std::is_default_constructible<Register>::value, "Register is not default constructible!");
	static_assert(assert_assignable<Register>::value, "Register is not assignable!");
	static_assert(std::is_destructible<Register>::value, "Register is not destructible!");

	static_assert(assert_assignable<Literal>::value, "Literal is not assignable!");
	static_assert(std::is_destructible<Literal>::value, "Literal is not destructible!");

	static_assert(assert_assignable<SmallImmediate>::value, "SmallImmediate is not assignable!");
	static_assert(std::is_destructible<SmallImmediate>::value, "SmallImmediate is not destructible!");

	static_assert(std::is_default_constructible<ContainerValue>::value, "ContainerValue is not default constructible!");
	static_assert(assert_assignable<ContainerValue>::value, "ContainerValue is not assignable!");
	static_assert(std::is_destructible<ContainerValue>::value, "ContainerValue is not destructible!");

	static_assert(assert_assignable<Value>::value, "Value is not assignable!");
	static_assert(std::is_destructible<Value>::value, "Value is not destructible!");

	static_assert(assert_not_copyable<Local>::value, "Local is copyable!");
	static_assert(std::is_move_constructible<Local>::value, "Local is not move constructible!");
	static_assert(std::is_destructible<Local>::value, "Local is not destructible!");

	/*
	 * Method/Module types
	 *
	 * (cannot be moved/copied, since it would invalidate references/pointers)
	 */
	static_assert(assert_not_assignable<BasicBlock>::value, "BasicBlock is assignable!");
	static_assert(std::is_destructible<BasicBlock>::value, "BasicBlock is not destructible!");

	static_assert(assert_not_assignable<Method>::value, "Method is assignable!");
	static_assert(std::is_destructible<Method>::value, "Method is not destructible!");

	static_assert(assert_not_assignable<Module>::value, "Module is assignable!");
	static_assert(std::is_destructible<Module>::value, "Module is not destructible!");

	static_assert(std::is_default_constructible<InstructionWalker>::value, "InstructionWalker is not default constructible!");
	static_assert(assert_assignable<InstructionWalker>::value, "InstructionWalker is not assignable!");
	static_assert(std::is_destructible<InstructionWalker>::value, "InstructionWalker is not destructible!");

	/*
	 * Intermediate instructions
	 *
	 * (live only on the heap, copying/moving them would invalidate the pointers e.g. for local-uses)
	 */
	static_assert(assert_not_assignable<intermediate::IntermediateInstruction>::value, "IntermediateInstruction is assignable!");
	static_assert(std::is_destructible<intermediate::IntermediateInstruction>::value, "IntermediateInstruction is not destructible!");

	/*
	 * Backend instructions and types
	 *
	 * (Try to keep fully assignable, if possible)
	 */
	static_assert(assert_assignable<ConditionCode>::value, "ConditionCode is not assignable!");
	static_assert(std::is_destructible<ConditionCode>::value, "ConditionCode is not destructible!");

	static_assert(assert_assignable<Signaling>::value, "Signaling is not assignable!");
	static_assert(std::is_destructible<Signaling>::value, "Signaling is not destructible!");

	static_assert(assert_assignable<Unpack>::value, "Unpack is not assignable!");
	static_assert(std::is_destructible<Unpack>::value, "Unpack is not destructible!");

	static_assert(assert_assignable<Pack>::value, "Pack is not assignable!");
	static_assert(std::is_destructible<Pack>::value, "Pack is not destructible!");

	static_assert(assert_assignable<OpCode>::value, "OpCode is not assignable!");
	static_assert(std::is_destructible<OpCode>::value, "OpCode is not destructible!");

	static_assert(assert_assignable<qpu_asm::ALUInstruction>::value, "ALUInstruction is not assignable!");
	static_assert(std::is_destructible<qpu_asm::ALUInstruction>::value, "ALUInstruction is not destructible!");

	/*
	 * Some helper types
	 *
	 * (Try to keep fully assignable, if possible)
	 */
	static_assert(std::is_default_constructible<Bitfield<uint64_t>>::value, "Bitfield is not default constructible!");
	static_assert(assert_assignable<Bitfield<uint64_t>>::value, "Bitfield is not assignable!");
	static_assert(std::is_destructible<Bitfield<uint64_t>>::value, "Bitfield is not destructible!");

	static_assert(assert_assignable<InstructionPart>::value, "InstructionPart is not assignable!");
	static_assert(std::is_destructible<InstructionPart>::value, "InstructionPart is not destructible!");

	static_assert(std::is_default_constructible<llvm2qasm::Token>::value, "Token is not default constructible!");
	static_assert(assert_assignable<llvm2qasm::Token>::value, "Token is not assignable!");
	static_assert(std::is_destructible<llvm2qasm::Token>::value, "Token is not destructible!");
}

#endif /* VC4C_CONCEPTS_H */
