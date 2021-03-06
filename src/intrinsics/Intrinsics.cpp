/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "Intrinsics.h"

#include "../intermediate/Helper.h"
#include "../intermediate/TypeConversions.h"
#include "../periphery/SFU.h"
#include "../periphery/TMU.h"
#include "../periphery/VPM.h"

#include "Comparisons.h"
#include "Images.h"
#include "Operators.h"
#include "log.h"

#include <cmath>
#include <cstdbool>
#include <map>
#include <vector>

using namespace vc4c;
using namespace vc4c::intermediate;


//The function to apply for pre-calculation
using UnaryInstruction = std::function<Optional<Value>(const Value&)>;
static const UnaryInstruction NO_OP = [](const Value& val){return NO_VALUE;};
//The function to apply for pre-calculation
using BinaryInstruction = std::function<Optional<Value>(const Value&, const Value&)>;
static const BinaryInstruction NO_OP2 = [](const Value& val0, const Value& val1){return NO_VALUE;};

//see VC4CLStdLib (_intrinsics.h)
static constexpr unsigned char VC4CL_UNSIGNED {1};


using IntrinsicFunction = std::function<InstructionWalker(Method&, InstructionWalker, const MethodCall*)>;
//NOTE: copying the captures is on purpose, since the sources do not exist anymore!

static IntrinsicFunction intrinsifyUnaryALUInstruction(const std::string& opCode, const bool useSignFlag = false, const Pack& packMode = PACK_NOP, const Unpack& unpackMode = UNPACK_NOP, bool setFlags = false)
{
	return [opCode, useSignFlag, packMode, unpackMode, setFlags](Method& method, InstructionWalker it, const MethodCall* callSite) -> InstructionWalker
	{
		bool isUnsigned = callSite->getArgument(1) && callSite->getArgument(1)->hasType(ValueType::LITERAL) && callSite->getArgument(1)->literal.integer == VC4CL_UNSIGNED;

		logging::debug() << "Intrinsifying unary '" << callSite->to_string() << "' to operation " << opCode << logging::endl;
		if(opCode == "mov")
			it.reset((new MoveOperation(callSite->getOutput().value(), callSite->getArgument(0).value()))->copyExtrasFrom(callSite));
		else
		it.reset((new Operation(opCode, callSite->getOutput().value(), callSite->getArgument(0).value()))->copyExtrasFrom(callSite));
		if(packMode != PACK_NOP)
			it->setPackMode(packMode);
		if(unpackMode != UNPACK_NOP)
			it->setUnpackMode(unpackMode);
		if(setFlags)
			it->setSetFlags(SetFlag::SET_FLAGS);

		if(useSignFlag && isUnsigned)
			it->setDecorations(InstructionDecorations::UNSIGNED_RESULT);

		return it;
	};
}

static IntrinsicFunction intrinsifyBinaryALUInstruction(const std::string& opCode, const bool useSignFlag = false, const Pack& packMode = PACK_NOP, const Unpack& unpackMode = UNPACK_NOP, bool setFlags = false)
{
	return [opCode, useSignFlag, packMode, unpackMode, setFlags](Method& method, InstructionWalker it, const MethodCall* callSite) -> InstructionWalker
	{
		bool isUnsigned = callSite->getArgument(2) && callSite->getArgument(2)->hasType(ValueType::LITERAL) && callSite->getArgument(2)->literal.integer == VC4CL_UNSIGNED;

		logging::debug() << "Intrinsifying binary '" << callSite->to_string() << "' to operation " << opCode << logging::endl;
		it.reset((new Operation(opCode, callSite->getOutput().value(), callSite->getArgument(0).value(), callSite->getArgument(1).value()))->copyExtrasFrom(callSite));
		if(packMode != PACK_NOP)
			it->setPackMode(packMode);
		if(unpackMode != UNPACK_NOP)
			it->setUnpackMode(unpackMode);
		if(setFlags)
			it->setSetFlags(SetFlag::SET_FLAGS);

		if(useSignFlag && isUnsigned)
			it->setDecorations(InstructionDecorations::UNSIGNED_RESULT);

		return it;
	};
}

static IntrinsicFunction intrinsifySFUInstruction(const Register& sfuRegister)
{
	return [sfuRegister](Method& method, InstructionWalker it, const MethodCall* callSite) -> InstructionWalker
	{
		logging::debug() << "Intrinsifying unary '" << callSite->to_string() << "' to SFU call" << logging::endl;
		it = periphery::insertSFUCall(sfuRegister, it, callSite->getArgument(0).value(), callSite->conditional);
		it.reset((new MoveOperation(callSite->getOutput().value(), Value(REG_SFU_OUT, callSite->getOutput()->type)))->copyExtrasFrom(callSite));
		return it;
	};
}

static IntrinsicFunction intrinsifyValueRead(const Value& val)
{
	return [val](Method& method, InstructionWalker it, const MethodCall* callSite) -> InstructionWalker
	{
		logging::debug() << "Intrinsifying method-call '" << callSite->to_string() << "' to value read" << logging::endl;
		it.reset((new MoveOperation(callSite->getOutput().value(), val))->copyExtrasFrom(callSite));
		return it;
	};
}

static IntrinsicFunction intrinsifySemaphoreAccess(bool increment)
{
	return [increment](Method& method, InstructionWalker it, const MethodCall* callSite) -> InstructionWalker
	{
		if(!callSite->getArgument(0)->hasType(ValueType::LITERAL))
			throw CompilationError(CompilationStep::OPTIMIZER, "Semaphore-number needs to be a compile-time constant", callSite->to_string());
		if(callSite->getArgument(0)->literal.integer < 0 || callSite->getArgument(0)->literal.integer >= 16)
			throw CompilationError(CompilationStep::OPTIMIZER, "Semaphore-number needs to be between 0 and 15", callSite->to_string());

		if(increment)
		{
			logging::debug() << "Intrinsifying semaphore increment with instruction" << logging::endl;
			it.reset((new SemaphoreAdjustment(static_cast<Semaphore>(callSite->getArgument(0)->literal.integer), true))->copyExtrasFrom(callSite));
		}
		else
		{
			logging::debug() << "Intrinsifying semaphore decrement with instruction" << logging::endl;
			it.reset((new SemaphoreAdjustment(static_cast<Semaphore>(callSite->getArgument(0)->literal.integer), false))->copyExtrasFrom(callSite));
		}
		return it;
	};
}

static IntrinsicFunction intrinsifyMutexAccess(bool lock)
{
	return [lock](Method& method, InstructionWalker it, const MethodCall* callSite) -> InstructionWalker
	{
		if(lock)
		{
			logging::debug() << "Intrinsifying mutex lock with instruction" << logging::endl;
			it.reset(new MutexLock(MutexAccess::LOCK));
		}
		else
		{
			logging::debug() << "Intrinsifying mutex unlock with instruction" << logging::endl;
			it.reset(new MutexLock(MutexAccess::RELEASE));
		}
		return it;
	};
}

enum class DMAAccess
{
	READ,
	WRITE,
	COPY,
	PREFETCH
};

static IntrinsicFunction intrinsifyDMAAccess(DMAAccess access)
{
	return [access](Method& method, InstructionWalker it, const MethodCall* callSite) -> InstructionWalker
	{
		switch(access)
		{
			case DMAAccess::READ:
			{
				logging::debug() << "Intrinsifying memory read " << callSite->to_string() << logging::endl;
				it = periphery::insertReadVectorFromTMU(method, it, callSite->getOutput().value(), callSite->getArgument(0).value());
				break;
			}
			case DMAAccess::WRITE:
			{
				logging::debug() << "Intrinsifying memory write " << callSite->to_string() << logging::endl;
				it = periphery::insertWriteDMA(method, it, callSite->getArgument(1).value(), callSite->getArgument(0).value(), false);
				break;
			}
			case DMAAccess::COPY:
			{
				logging::debug() << "Intrinsifying ternary '" << callSite->to_string() << "' to DMA copy operation " << logging::endl;
				const DataType type = callSite->getArgument(0)->type.getElementType();
				if(!callSite->getArgument(2) || !callSite->getArgument(2)->hasType(ValueType::LITERAL))
					throw CompilationError(CompilationStep::OPTIMIZER, "Memory copy with non-constant size is not yet supported", callSite->to_string());
				it = method.vpm->insertCopyRAM(method, it, callSite->getArgument(0).value(), callSite->getArgument(1).value(), callSite->getArgument(2)->literal.integer * type.getPhysicalWidth(), nullptr, false);
				break;
			}
			case DMAAccess::PREFETCH:
			{
				//TODO could be used to load into VPM and then use the cache for further reads
				//for now, simply discard
				logging::debug() << "Discarding unsupported DMA pre-fetch: " << callSite->to_string() << logging::endl;
				break;
			}
		}

		it.erase();
		//so next instruction is not skipped
		it.previousInBlock();

		return it;
	};
}

static IntrinsicFunction intrinsifyVectorRotation()
{
	return [](Method& method, InstructionWalker it, const MethodCall* callSite) -> InstructionWalker
	{
		logging::debug() << "Intrinsifying vector rotation " << callSite->to_string() << logging::endl;
		it = insertVectorRotation(it, callSite->getArgument(0).value(), callSite->getArgument(1).value(), callSite->getOutput().value(), Direction::UP);
		it.erase();
		//so next instruction is not skipped
		it.previousInBlock();

		return it;
	};
}

struct Intrinsic
{
	const IntrinsicFunction func;
    const Optional<UnaryInstruction> unaryInstr;
    const Optional<BinaryInstruction> binaryInstr;
    
    explicit Intrinsic(const IntrinsicFunction& func): func(func) { }
    
    Intrinsic(const IntrinsicFunction& func, const UnaryInstruction unary) : func(func), unaryInstr(unary) { }
    
    Intrinsic(const IntrinsicFunction& func, const BinaryInstruction binary) : func(func), binaryInstr(binary) { }
};

const static std::map<std::string, Intrinsic> nonaryInstrinsics = {
    {"vc4cl_mutex_lock", Intrinsic{intrinsifyMutexAccess(true)}},
    {"vc4cl_mutex_unlock", Intrinsic{intrinsifyMutexAccess(false)}},
	{"vc4cl_element_number", Intrinsic{intrinsifyValueRead(ELEMENT_NUMBER_REGISTER)}},
	{"vc4cl_qpu_number", Intrinsic{intrinsifyValueRead(Value(REG_QPU_NUMBER, TYPE_INT8))}}
};

const static std::map<std::string, Intrinsic> unaryIntrinsicMapping = {
    {"vc4cl_ftoi", Intrinsic{intrinsifyUnaryALUInstruction(OP_FTOI.name), [](const Value& val){return Value(Literal(static_cast<int64_t>(std::round(val.literal.real()))), TYPE_INT32);}}},
    {"vc4cl_itof", Intrinsic{intrinsifyUnaryALUInstruction(OP_ITOF.name), [](const Value& val){return Value(Literal(static_cast<double>(val.literal.integer)), TYPE_FLOAT);}}},
    {"vc4cl_clz", Intrinsic{intrinsifyUnaryALUInstruction(OP_CLZ.name), NO_OP}},
    {"vc4cl_sfu_rsqrt", Intrinsic{intrinsifySFUInstruction(REG_SFU_RECIP_SQRT), [](const Value& val){return Value(Literal(1.0 / std::sqrt(val.literal.real())), TYPE_FLOAT);}}},
    {"vc4cl_sfu_exp2", Intrinsic{intrinsifySFUInstruction(REG_SFU_EXP2), [](const Value& val){return Value(Literal(std::exp2(val.literal.real())), TYPE_FLOAT);}}},
    {"vc4cl_sfu_log2", Intrinsic{intrinsifySFUInstruction(REG_SFU_LOG2), [](const Value& val){return Value(Literal(std::log2(val.literal.real())), TYPE_FLOAT);}}},
    {"vc4cl_sfu_recip", Intrinsic{intrinsifySFUInstruction(REG_SFU_RECIP), [](const Value& val){return Value(Literal(1.0 / val.literal.real()), TYPE_FLOAT);}}},
    {"vc4cl_semaphore_increment", Intrinsic{intrinsifySemaphoreAccess(true)}},
    {"vc4cl_semaphore_decrement", Intrinsic{intrinsifySemaphoreAccess(false)}},
    {"vc4cl_dma_read", Intrinsic{intrinsifyDMAAccess(DMAAccess::READ)}},
	{"vc4cl_unpack_sext", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_NOP, UNPACK_SHORT_TO_INT_SEXT)}},
	{"vc4cl_unpack_color_byte0", Intrinsic{intrinsifyUnaryALUInstruction(OP_FMIN.name, false, PACK_NOP, UNPACK_8A_32)}},
	{"vc4cl_unpack_color_byte1", Intrinsic{intrinsifyUnaryALUInstruction(OP_FMIN.name, false, PACK_NOP, UNPACK_8B_32)}},
	{"vc4cl_unpack_color_byte2", Intrinsic{intrinsifyUnaryALUInstruction(OP_FMIN.name, false, PACK_NOP, UNPACK_8C_32)}},
	{"vc4cl_unpack_color_byte3", Intrinsic{intrinsifyUnaryALUInstruction(OP_FMIN.name, false, PACK_NOP, UNPACK_8D_32)}},
    {"vc4cl_unpack_byte0", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_NOP, UNPACK_8A_32)}},
	{"vc4cl_unpack_byte1", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_NOP, UNPACK_8B_32)}},
	{"vc4cl_unpack_byte2", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_NOP, UNPACK_8C_32)}},
	{"vc4cl_unpack_byte3", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_NOP, UNPACK_8D_32)}},
	{"vc4cl_pack_truncate", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_INT_TO_SHORT_TRUNCATE)}},
	{"vc4cl_replicate_lsb", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_32_8888)}},
	{"vc4cl_pack_lsb", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_INT_TO_CHAR_TRUNCATE)}},
	{"vc4cl_saturate_short", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_INT_TO_SIGNED_SHORT_SATURATE)}},
	{"vc4cl_saturate_lsb", Intrinsic{intrinsifyUnaryALUInstruction("mov", false, PACK_INT_TO_UNSIGNED_CHAR_SATURATE)}}
};

const static std::map<std::string, Intrinsic> binaryIntrinsicMapping = {
    {"vc4cl_fmax", Intrinsic{intrinsifyBinaryALUInstruction(OP_FMAX.name), [](const Value& val0, const Value& val1){return Value(Literal(std::max(val0.literal.real(), val1.literal.real())), TYPE_FLOAT);}}},
    {"vc4cl_fmin", Intrinsic{intrinsifyBinaryALUInstruction(OP_FMIN.name), [](const Value& val0, const Value& val1){return Value(Literal(std::min(val0.literal.real(), val1.literal.real())), TYPE_FLOAT);}}},
    {"vc4cl_fmaxabs", Intrinsic{intrinsifyBinaryALUInstruction(OP_FMAXABS.name), [](const Value& val0, const Value& val1){return Value(Literal(std::max(std::abs(val0.literal.real()), std::abs(val1.literal.real()))), TYPE_FLOAT);}}},
    {"vc4cl_fminabs", Intrinsic{intrinsifyBinaryALUInstruction(OP_FMINABS.name), [](const Value& val0, const Value& val1){return Value(Literal(std::min(std::abs(val0.literal.real()), std::abs(val1.literal.real()))), TYPE_FLOAT);}}},
	//FIXME sign / no-sign!!
    {"vc4cl_shr", Intrinsic{intrinsifyBinaryALUInstruction(OP_SHR.name), [](const Value& val0, const Value& val1){return Value(Literal(val0.literal.integer >> val1.literal.integer), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_asr", Intrinsic{intrinsifyBinaryALUInstruction(OP_ASR.name), [](const Value& val0, const Value& val1){return Value(Literal(val0.literal.integer >> val1.literal.integer), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_ror", Intrinsic{intrinsifyBinaryALUInstruction(OP_ROR.name), NO_OP2}},
    {"vc4cl_shl", Intrinsic{intrinsifyBinaryALUInstruction(OP_SHL.name), [](const Value& val0, const Value& val1){return Value(Literal(val0.literal.integer << val1.literal.integer), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_min", Intrinsic{intrinsifyBinaryALUInstruction(OP_MIN.name, true), [](const Value& val0, const Value& val1){return Value(Literal(std::min(val0.literal.integer, val1.literal.integer)), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_max", Intrinsic{intrinsifyBinaryALUInstruction(OP_MAX.name, true), [](const Value& val0, const Value& val1){return Value(Literal(std::max(val0.literal.integer, val1.literal.integer)), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_and", Intrinsic{intrinsifyBinaryALUInstruction(OP_AND.name), [](const Value& val0, const Value& val1){return Value(Literal(val0.literal.integer & val1.literal.integer), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_mul24", Intrinsic{intrinsifyBinaryALUInstruction(OP_MUL24.name, true), [](const Value& val0, const Value& val1){return Value(Literal((val0.literal.integer & 0xFFFFFFL) * (val1.literal.integer & 0xFFFFFFL)), val0.type.getUnionType(val1.type));}}},
    {"vc4cl_dma_write", Intrinsic{intrinsifyDMAAccess(DMAAccess::WRITE)}},
	{"vc4cl_vector_rotate", Intrinsic{intrinsifyVectorRotation()}},
	//XXX correct, can use the flags emitted by the very same instruction?
	{"vc4cl_saturated_add", Intrinsic{intrinsifyBinaryALUInstruction(OP_ADD.name, false, PACK_32_32, UNPACK_NOP, true)}},
	{"vc4cl_saturated_sub", Intrinsic{intrinsifyBinaryALUInstruction(OP_SUB.name, false, PACK_32_32, UNPACK_NOP, true)}},
};

const static std::map<std::string, Intrinsic> ternaryIntrinsicMapping = {
	{"vc4cl_dma_copy", Intrinsic{intrinsifyDMAAccess(DMAAccess::COPY)}}
};

const static std::map<std::string, std::pair<Intrinsic, Optional<Value>>> typeCastIntrinsics = {
	//since we run all the (not intrinsified) calculations with 32-bit, don't truncate signed conversions to smaller types
	//TODO correct?? Since we do not discard out-of-bounds values!
    {"vc4cl_bitcast_uchar", {Intrinsic{intrinsifyBinaryALUInstruction("and", true), [](const Value& val){return Value(Literal(val.literal.integer & 0xFF), TYPE_INT8);}}, Value(Literal(static_cast<uint64_t>(0xFF)), TYPE_INT8)}},
    {"vc4cl_bitcast_char", {Intrinsic{intrinsifyBinaryALUInstruction("mov"), [](const Value& val){return Value(val.literal, TYPE_INT8);}}, NO_VALUE}},
    {"vc4cl_bitcast_ushort", {Intrinsic{intrinsifyBinaryALUInstruction("and", true), [](const Value& val){return Value(Literal(val.literal.integer & 0xFFFF), TYPE_INT16);}}, Value(Literal(static_cast<uint64_t>(0xFFFF)), TYPE_INT16)}},
    {"vc4cl_bitcast_short", {Intrinsic{intrinsifyBinaryALUInstruction("mov"), [](const Value& val){return Value(val.literal, TYPE_INT16);}}, NO_VALUE}},
    {"vc4cl_bitcast_uint", {Intrinsic{intrinsifyBinaryALUInstruction("mov", true), [](const Value& val){return Value(Literal(val.literal.integer & static_cast<int64_t>(0xFFFFFFFF)), TYPE_INT32);}}, NO_VALUE}},
    {"vc4cl_bitcast_int", {Intrinsic{intrinsifyBinaryALUInstruction("mov"), [](const Value& val){return Value(val.literal, TYPE_INT32);}}, NO_VALUE}},
    {"vc4cl_bitcast_float", {Intrinsic{intrinsifyBinaryALUInstruction("mov"), [](const Value& val){return Value(Literal(val.literal.integer & static_cast<int64_t>(0xFFFFFFFF)), TYPE_INT32);}}, NO_VALUE}}
};

static InstructionWalker intrinsifyNoArgs(Method& method, InstructionWalker it)
{
    MethodCall* callSite = it.get<MethodCall>();
    if(callSite == nullptr)
    {
        return it;
    }
    if(callSite->getArguments().size() > 1 /* check for sign-flag too*/)
    {
        return it;
    }
    for(const auto& pair : nonaryInstrinsics)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
        	return pair.second.func(method, it, callSite);
        }
    }
    return it;
}

static InstructionWalker intrinsifyUnary(Method& method, InstructionWalker it)
{
    MethodCall* callSite = it.get<MethodCall>();
    if(callSite == nullptr)
    {
        return it;
    }
    if(callSite->getArguments().size() == 0 || callSite->getArguments().size() > 2 /* check for sign-flag too*/)
    {
        return it;
    }
    for(const auto& pair : unaryIntrinsicMapping)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
        	if(callSite->getArgument(0)->hasType(ValueType::LITERAL) && pair.second.unaryInstr && pair.second.unaryInstr.value()(callSite->getArgument(0).value()))
        	{
        		logging::debug() << "Intrinsifying unary '" << callSite->to_string() << "' to pre-calculated value" << logging::endl;
        		it.reset(new MoveOperation(callSite->getOutput().value(), pair.second.unaryInstr.value()(callSite->getArgument(0).value()).value(), callSite->conditional, callSite->setFlags));
        	}
        	else
        	{
        		return pair.second.func(method, it, callSite);
        	}
            return it;
        }
    }
    for(const auto& pair : typeCastIntrinsics)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
        	if(callSite->getArgument(0)->hasType(ValueType::LITERAL) && pair.second.first.unaryInstr && pair.second.first.unaryInstr.value()(callSite->getArgument(0).value()))
			{
				logging::debug() << "Intrinsifying type-cast '" << callSite->to_string() << "' to pre-calculated value" << logging::endl;
				it.reset(new MoveOperation(callSite->getOutput().value(), pair.second.first.unaryInstr.value()(callSite->getArgument(0).value()).value(), callSite->conditional, callSite->setFlags));
			}
        	else if(!pair.second.second)	//there is no value to apply -> simple move
        	{
        		logging::debug() << "Intrinsifying '" << callSite->to_string() << "' to simple move" << logging::endl;
				it.reset(new MoveOperation(callSite->getOutput().value(), callSite->getArgument(0).value()));
        	}
        	else
            {
        		//TODO could use pack-mode here, but only for UNSIGNED values!!
				logging::debug() << "Intrinsifying '" << callSite->to_string() << "' to operation with constant " << pair.second.second.to_string() << logging::endl;
				callSite->setArgument(1, pair.second.second.value());
				return pair.second.first.func(method, it, callSite);
            }
            return it;
        }
    }
    return it;
}

static InstructionWalker intrinsifyBinary(Method& method, InstructionWalker it)
{
    MethodCall* callSite = it.get<MethodCall>();
    if(callSite == nullptr)
    {
        return it;
    }
    if(callSite->getArguments().size() < 2 || callSite->getArguments().size() > 3 /* check for sign-flag too*/)
    {
        return it;
    }
    for(const auto& pair : binaryIntrinsicMapping)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
        	if(callSite->getArgument(0)->hasType(ValueType::LITERAL) && callSite->getArgument(1)->hasType(ValueType::LITERAL) && pair.second.binaryInstr && pair.second.binaryInstr.value()(callSite->getArgument(0).value(), callSite->getArgument(1).value()))
			{
				logging::debug() << "Intrinsifying binary '" << callSite->to_string() << "' to pre-calculated value" << logging::endl;
				it.reset(new MoveOperation(callSite->getOutput().value(), pair.second.binaryInstr.value()(callSite->getArgument(0).value(), callSite->getArgument(1).value()).value(), callSite->conditional, callSite->setFlags));
			}
        	else
        	{
        		return pair.second.func(method, it, callSite);
        	}
            return it;
        }
    }
    return it;
}

static InstructionWalker intrinsifyTernary(Method& method, InstructionWalker it)
{
    MethodCall* callSite = it.get<MethodCall>();
    if(callSite == nullptr)
    {
        return it;
    }
    if(callSite->getArguments().size() < 3 || callSite->getArguments().size() > 4 /* check for sign-flag too*/)
    {
        return it;
    }
    for(const auto& pair : ternaryIntrinsicMapping)
    {
        if(callSite->methodName.find(pair.first) != std::string::npos)
        {
        	return pair.second.func(method, it, callSite);
        }
    }
    return it;
}

static bool isPowerTwo(int64_t val)
{
    //https://en.wikipedia.org/wiki/Power_of_two#Fast_algorithm_to_check_if_a_positive_number_is_a_power_of_two
    return val > 0 && (val & (val - 1)) == 0;
}

static InstructionWalker intrinsifyArithmetic(Method& method, InstructionWalker it, const MathType& mathType)
{
    Operation* op = it.get<Operation>();
    if(op == nullptr)
    {
        return it;
    }
    const Value& arg0 = op->getFirstArg();
    const Value& arg1 = op->getSecondArg().value_or(UNDEFINED_VALUE);
    const bool saturateResult = has_flag(op->decoration, InstructionDecorations::SATURATED_CONVERSION);
    //integer multiplication
    if(op->opCode == "mul")
    {
        //a * b = b * a
        //a * 2^n = a << n
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for multiplication with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput()->local, arg0.type), Value(Literal(arg0.literal.integer * arg1.literal.integer), arg0.type), op->conditional, op->setFlags));
        }
        else if(arg0.hasType(ValueType::LITERAL) && isPowerTwo(arg0.literal.integer))
        {
            logging::debug() << "Intrinsifying multiplication with left-shift" << logging::endl;
            op->setOpCode(OP_SHL);
            op->setArgument(0, arg1);
            op->setArgument(1, Value(Literal(static_cast<int64_t>(std::log2(arg0.literal.integer))), arg0.type));
        }
        else if(arg1.hasType(ValueType::LITERAL) && isPowerTwo(arg1.literal.integer))
        {
            logging::debug() << "Intrinsifying multiplication with left-shift" << logging::endl;
            op->setOpCode(OP_SHL);
            op->setArgument(1, Value(Literal(static_cast<int64_t>(std::log2(arg1.literal.integer))), arg1.type));
        }
        else if(std::max(arg0.type.getScalarBitCount(), arg1.type.getScalarBitCount()) <= 24)
        {
            logging::debug() << "Intrinsifying multiplication of small integers to mul24" << logging::endl;
            op->setOpCode(OP_MUL24);
        }
        else
        {
            it = intrinsifySignedIntegerMultiplication(method, it, *op);
        }
    }
    //unsigned division
    else if(op->opCode == "udiv")
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for division with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput()->local, arg0.type), Value(Literal(static_cast<uint64_t>(arg0.literal.integer / arg1.literal.integer)), arg0.type), op->conditional, op->setFlags));
        }
        //a / 2^n = a >> n
        else if(arg1.hasType(ValueType::LITERAL) && isPowerTwo(arg1.literal.integer))
        {
            logging::debug() << "Intrinsifying division with right-shift" << logging::endl;
            op->setOpCode(OP_SHR);
            op->setArgument(1, Value(Literal(static_cast<int64_t>(std::log2(arg1.literal.integer))), arg1.type));
        }
        else if((arg1.isLiteralValue() || arg1.hasType(ValueType::CONTAINER)) && arg0.type.getScalarBitCount() <= 16)
        {
        	it = intrinsifyUnsignedIntegerDivisionByConstant(method, it, *op);
        }
        else
        {
            it = intrinsifyUnsignedIntegerDivision(method, it, *op);
        }
    }
    //signed division
    else if(op->opCode == "sdiv")
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for signed division with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput()->local, arg0.type), Value(Literal(arg0.literal.integer / arg1.literal.integer), arg0.type), op->conditional, op->setFlags));
        }
        //a / 2^n = a >> n
        else if(arg1.hasType(ValueType::LITERAL) && isPowerTwo(arg1.literal.integer))
        {
            logging::debug() << "Intrinsifying signed division with arithmetic right-shift" << logging::endl;
            op->setOpCode(OP_ASR);
            op->setArgument(1, Value(Literal(static_cast<int64_t>(std::log2(arg1.literal.integer))), arg1.type));
        }
        else if((arg1.isLiteralValue() || arg1.hasType(ValueType::CONTAINER)) && arg0.type.getScalarBitCount() <= 16)
        {
        	it = intrinsifySignedIntegerDivisionByConstant(method, it, *op);
        }
        else
        {
            it = intrinsifySignedIntegerDivision(method, it, *op);
        }
    }
    //unsigned modulo
    //LLVM IR calls it urem, SPIR-V umod
    else if(op->opCode == "urem" || op->opCode == "umod")
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for modulo with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput()->local, arg0.type), Value(Literal(static_cast<uint64_t>(arg0.literal.integer % arg1.literal.integer)), arg0.type), op->conditional, op->setFlags));
        }
        else if(arg1.hasType(ValueType::LITERAL) && isPowerTwo(arg1.literal.integer))
        {
            logging::debug() << "Intrinsifying unsigned modulo by power of two" << logging::endl;
            op->setOpCode(OP_AND);
            op->setArgument(1, Value(Literal(arg1.literal.integer - 1), arg1.type));
        }
        else if((arg1.isLiteralValue() || arg1.hasType(ValueType::CONTAINER)) && arg0.type.getScalarBitCount() <= 16)
		{
			it = intrinsifyUnsignedIntegerDivisionByConstant(method, it, *op, true);
		}
        else
        {
            it = intrinsifyUnsignedIntegerDivision(method, it, *op, true);
        }
    }
    //signed modulo
    else if(op->opCode == "srem")
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for signed modulo with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput()->local, arg0.type), Value(Literal(arg0.literal.integer % arg1.literal.integer), arg0.type), op->conditional, op->setFlags));
        }
        else if((arg1.isLiteralValue() || arg1.hasType(ValueType::CONTAINER)) && arg0.type.getScalarBitCount() <= 16)
		{
			it = intrinsifySignedIntegerDivisionByConstant(method, it, *op, true);
		}
        else
        {
            it = intrinsifySignedIntegerDivision(method, it, *op, true);
        }
    }
    //floating division
    else if(op->opCode == "fdiv")
    {
        if(arg0.hasType(ValueType::LITERAL) && arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Calculating result for signed division with constants" << logging::endl;
            it.reset(new MoveOperation(Value(op->getOutput()->local, arg0.type), Value(Literal(arg0.literal.real() / arg1.literal.real()), arg0.type), op->conditional, op->setFlags));
        }
        else if(arg1.hasType(ValueType::LITERAL))
        {
            logging::debug() << "Intrinsifying floating division with multiplication of constant inverse" << logging::endl;
            op->setOpCode(OP_FMUL);
            op->setArgument(1, Value(Literal(1.0 / arg1.literal.real()), arg1.type));
        }
        else if(has_flag(op->decoration, InstructionDecorations::ALLOW_RECIP) || has_flag(op->decoration, InstructionDecorations::FAST_MATH))
        {
            logging::debug() << "Intrinsifying floating division with multiplication of reciprocal" << logging::endl;
            it = periphery::insertSFUCall(REG_SFU_RECIP, it, arg1, op->conditional);
            it.nextInBlock();
            op->setOpCode(OP_FMUL);
            op->setArgument(1, Value(REG_SFU_OUT, op->getFirstArg().type));
        }
        else
        {
            logging::debug() << "Intrinsifying floating division with multiplication of inverse" << logging::endl;
            it = intrinsifyFloatingDivision(method, it, *op);
        }
    }
    //truncate bits
    else if(op->opCode == "trunc")
    {
    	if(saturateResult)
    	{
    		//let pack-mode handle saturation
    		logging::debug() << "Intrinsifying saturated truncate with move and pack-mode" << logging::endl;
    		it = insertSaturation(it, method, op->getFirstArg(), op->getOutput().value(), !has_flag(op->decoration, InstructionDecorations::UNSIGNED_RESULT));
    		it.nextInBlock();
    		it.erase();
    	}
        //if orig = i64, dest = i32 -> move
    	else if(op->getFirstArg().type.getScalarBitCount() > 32 && op->getOutput()->type.getScalarBitCount() == 32)
        {
            //do nothing, is just a move, since we truncate the 64-bit integers anyway
            logging::debug() << "Intrinsifying truncate from unsupported type with move" << logging::endl;
            it.reset((new MoveOperation(op->getOutput().value(), op->getFirstArg(), op->conditional, op->setFlags))->copyExtrasFrom(op));
        }
        //if dest < i32 -> orig & dest-bits or pack-code
        else if(op->getOutput()->type.getScalarBitCount() < 32)
        {
            logging::debug() << "Intrinsifying truncate with and" << logging::endl;
            op->setOpCode(OP_AND);
            op->setArgument(1, Value(Literal(op->getOutput()->type.getScalarWidthMask()), TYPE_INT32));
        }
    }
    else if(op->opCode == "fptrunc")
    {
    	if(saturateResult)
    	{
    		throw CompilationError(CompilationStep::OPTIMIZER, "Saturation on floating-point conversion is not supprted", op->to_string());
    	}
        it = insertFloatingPointConversion(it, method, arg0, op->getOutput().value());
        //remove 'fptrunc'
        it.erase();
        //so next instruction is not skipped
		it.previousInBlock();
    }
    //arithmetic shift right
    else if(op->opCode == "ashr")
    {
        //just surgical modification
        op->setOpCode(OP_ASR);
    }
    else if(op->opCode == "lshr")
    {
        //TODO only if type <= i32 and/or offset <= 32
        //just surgical modification
        op->setOpCode(OP_SHR);
    }
    //integer to float
    else if(op->opCode == "sitofp")
    {
    	//for non 32-bit types, need to sign-extend
    	Value tmp = op->getFirstArg();
    	if(op->getFirstArg().type.getScalarBitCount() < 32)
    	{
    		tmp = method.addNewLocal(TYPE_INT32, "%sitofp");
    		it = insertSignExtension(it, method, op->getFirstArg(), tmp, true, op->conditional);
    	}
        //just surgical modification
        op->setOpCode(OP_ITOF);
        if(tmp != op->getFirstArg())
        	op->setArgument(0, tmp);
    }
    else if(op->opCode == "uitofp")
    {
    	const Value tmp = method.addNewLocal(op->getOutput()->type, "%uitofp");
        if(op->getFirstArg().type.getScalarBitCount() < 32)
        {
            //make sure, leading bits are zeroes
            const int64_t mask = op->getFirstArg().type.getScalarWidthMask();
            it.emplace(new Operation(OP_AND, tmp, op->getFirstArg(), Value(Literal(mask), TYPE_INT32), op->conditional));
            it.nextInBlock();
            op->setArgument(0, tmp);
            op->setOpCode(OP_ITOF);
        }
        else if(op->getFirstArg().type.getScalarBitCount() > 32)
        {
            throw CompilationError(CompilationStep::OPTIMIZER, "Can't convert long to floating value, since long is not supported!");
        }
        else    //32-bits
        {
            //itofp + if MSB set add 2^31(f)
//            it.emplace(new Operation("and", REG_NOP, Value(Literal(0x80000000UL), TYPE_INT32), op->getFirstArg(), op->conditional, SetFlag::SET_FLAGS));
//            ++it;
//            it.emplace((new Operation("itof", tmp, op->getFirstArg(), op->conditional))->setDecorations(op->decoration));
//            ++it;
//            it.reset(new Operation("fadd", op->getOutput(), tmp, Value(Literal(std::pow(2, 31)), TYPE_FLOAT), COND_ZERO_CLEAR));
        	//TODO this passed OpenCL-CTS parameter_types, but what of large values (MSB set)??
        	op->setOpCode(OP_ITOF);
        }
    }
    //float to integer
    else if(op->opCode == "fptosi")
    {
        //just surgical modification
        op->setOpCode(OP_FTOI);
    }
    //float to unsigned integer
    else if(op->opCode == "fptoui")
    {
        //TODO special treatment??
    	//TODO truncate to type?
        op->setOpCode(OP_FTOI);
        op->decoration = add_flag(op->decoration, InstructionDecorations::UNSIGNED_RESULT);
    }
    //sign extension
    else if(op->opCode == "sext")
    {
        logging::debug() << "Intrinsifying sign extension with shifting" << logging::endl;
        it = insertSignExtension(it, method, op->getFirstArg(), op->getOutput().value(), true, op->conditional, op->setFlags);
        //remove 'sext'
        it.erase();
        //so next instruction is not skipped
        it.previousInBlock();
    }
    //zero extension
    else if(op->opCode == "zext")
    {
        logging::debug() << "Intrinsifying zero extension with and" << logging::endl;
        it = insertZeroExtension(it, method, op->getFirstArg(), op->getOutput().value(), true, op->conditional, op->setFlags);
        //remove 'zext'
        it.erase();
        //so next instruction is not skipped
        it.previousInBlock();
    }
    return it;
}

static InstructionWalker intrinsifyReadWorkGroupInfo(Method& method, InstructionWalker it, const Value& arg, const std::vector<std::string>& locals, const Value& defaultValue, const InstructionDecorations decoration)
{
	if(arg.hasType(ValueType::LITERAL))
	{
		Value src = UNDEFINED_VALUE;
		switch(arg.literal.integer)
		{
			case 0:
				src = method.findOrCreateLocal(TYPE_INT32, locals.at(0))->createReference();
				break;
			case 1:
				src = method.findOrCreateLocal(TYPE_INT32, locals.at(1))->createReference();
				break;
			case 2:
				src = method.findOrCreateLocal(TYPE_INT32, locals.at(2))->createReference();
				break;
			default:
				src = defaultValue;
		}
		return it.reset((new MoveOperation(it->getOutput().value(), src))->copyExtrasFrom(it.get()));
	}
	//set default value first and always, so a path for the destination local is guaranteed
	it.emplace(new MoveOperation(it->getOutput().value(), defaultValue));
	it.nextInBlock();
	//dim == 0 -> return first value
	it.emplace((new Operation(OP_XOR, NOP_REGISTER, arg, INT_ZERO))->setSetFlags(SetFlag::SET_FLAGS));
	it.nextInBlock();
	it.emplace(new MoveOperation(it->getOutput().value(), method.findOrCreateLocal(TYPE_INT32, locals.at(0))->createReference(), COND_ZERO_SET));
	it.nextInBlock();
	//dim == 1 -> return second value
	it.emplace((new Operation(OP_XOR, NOP_REGISTER, arg, INT_ONE))->setSetFlags(SetFlag::SET_FLAGS));
	it.nextInBlock();
	it.emplace(new MoveOperation(it->getOutput().value(), method.findOrCreateLocal(TYPE_INT32, locals.at(1))->createReference(), COND_ZERO_SET));
	it.nextInBlock();
	//dim == 2 -> return third value
	it.emplace((new Operation(OP_XOR, NOP_REGISTER, arg, Value(Literal(static_cast<int64_t>(2)), TYPE_INT32)))->setSetFlags(SetFlag::SET_FLAGS));
	it.nextInBlock();
	return it.reset(new MoveOperation(it->getOutput().value(), method.findOrCreateLocal(TYPE_INT32, locals.at(2))->createReference(), COND_ZERO_SET));
}

static InstructionWalker intrinsifyReadWorkItemInfo(Method& method, InstructionWalker it, const Value& arg, const std::string& local, const InstructionDecorations decoration)
{
	/*
	 * work-item infos (id, size) are stored within a single UNIFORM:
	 * high <-> low byte
	 * 00 | 3.dim | 2.dim | 1.dim
	 * -> res = (UNIFORM >> (dim * 8)) & 0xFF
	 */
	const Local* itemInfo = method.findOrCreateLocal(TYPE_INT32, local);
	Value tmp0 = method.addNewLocal(TYPE_INT32, "%local_info");
	it.emplace(new Operation(OP_MUL24, tmp0, arg, Value(Literal(static_cast<int64_t>(8)), TYPE_INT32)));
	it.nextInBlock();
	const Value tmp1 = method.addNewLocal(TYPE_INT32, "%local_info");
	it.emplace(new Operation(OP_SHR, tmp1, itemInfo->createReference(), tmp0));
	it.nextInBlock();
	return it.reset((new Operation(OP_AND, it->getOutput().value(), tmp1, Value(Literal(static_cast<int64_t>(0xFF)), TYPE_INT8)))->copyExtrasFrom(it.get()));
}

static InstructionWalker intrinsifyWorkItemFunctions(Method& method, InstructionWalker it)
{
	MethodCall* callSite = it.get<MethodCall>();
	if(callSite == nullptr)
		return it;
	if(callSite->getArguments().size() > 1)
		return it;

	if(callSite->methodName.compare("vc4cl_work_dimensions") == 0 && callSite->getArguments().size() == 0)
	{
		logging::debug() << "Intrinsifying reading of work-item dimensions" << logging::endl;
		//setting the type to int8 allows us to optimize e.g. multiplications with work-item values
		Value out = callSite->getOutput().value();
		out.type = TYPE_INT8;
		return it.reset((new MoveOperation(out, method.findOrCreateLocal(TYPE_INT32, Method::WORK_DIMENSIONS)->createReference()))->copyExtrasFrom(callSite)->setDecorations(add_flag(callSite->decoration, InstructionDecorations::BUILTIN_WORK_DIMENSIONS)));
	}
	if(callSite->methodName == "vc4cl_num_groups" && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of the number of work-groups" << logging::endl;
		return intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0).value(), {Method::NUM_GROUPS_X, Method::NUM_GROUPS_Y, Method::NUM_GROUPS_Z}, INT_ONE, InstructionDecorations::BUILTIN_NUM_GROUPS);
	}
	if(callSite->methodName == "vc4cl_group_id" && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of the work-group ids" << logging::endl;
		return intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0).value(), {Method::GROUP_ID_X, Method::GROUP_ID_Y, Method::GROUP_ID_Z}, INT_ZERO, InstructionDecorations::BUILTIN_GROUP_ID);
	}
	if(callSite->methodName == "vc4cl_global_offset" && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of the global offsets" << logging::endl;
		return intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0).value(), {Method::GLOBAL_OFFSET_X, Method::GLOBAL_OFFSET_Y, Method::GLOBAL_OFFSET_Z}, INT_ZERO, InstructionDecorations::BUILTIN_GLOBAL_OFFSET);
	}
	if(callSite->methodName == "vc4cl_local_size" && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of local work-item sizes" << logging::endl;
		/*
		 * Use the value set via reqd_work_group_size(x, y, z) - if set - and return here.
		 * This is valid, since the OpenCL standard states: "is the work-group size that must be used as the local_work_size argument to clEnqueueNDRangeKernel." (page 231)
		 */
		const auto& arg0 = callSite->getArgument(0).value();
		const auto& workGroupSizes = method.metaData.workGroupSizes;
		if(workGroupSizes.at(0) > 0 && arg0.isLiteralValue())
		{
			const Literal immediate = arg0.getLiteralValue().value();
			if(immediate.integer > static_cast<int64_t>(workGroupSizes.size()) || workGroupSizes.at(immediate.integer) == 0)
			{
				return it.reset((new MoveOperation(callSite->getOutput().value(), INT_ONE))->setDecorations(InstructionDecorations::BUILTIN_LOCAL_SIZE));
			}
			return it.reset((new MoveOperation(callSite->getOutput().value(), Value(Literal(static_cast<uint64_t>(workGroupSizes.at(immediate.integer))), TYPE_INT8)))->setDecorations(InstructionDecorations::BUILTIN_LOCAL_SIZE));
		}
		//TODO needs to have a size of 1 for all higher dimensions (instead of currently implicit 0)
		return intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0).value(), Method::LOCAL_SIZES, InstructionDecorations::BUILTIN_LOCAL_SIZE);
	}
	if(callSite->methodName == "vc4cl_local_id" && callSite->getArguments().size() == 1)
	{
		logging::debug() << "Intrinsifying reading of local work-item ids" << logging::endl;
		return intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0).value(), Method::LOCAL_IDS, InstructionDecorations::BUILTIN_LOCAL_ID);
	}
	if(callSite->methodName == "vc4cl_global_size" && callSite->getArguments().size() == 1)
	{
		//global_size(dim) = local_size(dim) * num_groups(dim)
		logging::debug() << "Intrinsifying reading of global work-item sizes" << logging::endl;

		const Value tmpLocalSize = method.addNewLocal(TYPE_INT8, "%local_size");
		const Value tmpNumGroups = method.addNewLocal(TYPE_INT32, "%num_groups");
		//emplace dummy instructions to be replaced
		it.emplace(new MoveOperation(tmpLocalSize, NOP_REGISTER));
		it = intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0).value(), Method::LOCAL_SIZES, InstructionDecorations::BUILTIN_LOCAL_SIZE);
		it.nextInBlock();
		it.emplace(new MoveOperation(tmpNumGroups, NOP_REGISTER));
		it = intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0).value(), {Method::NUM_GROUPS_X, Method::NUM_GROUPS_Y, Method::NUM_GROUPS_Z}, INT_ONE, InstructionDecorations::BUILTIN_NUM_GROUPS);
		it.nextInBlock();
		return it.reset((new Operation(OP_MUL24, callSite->getOutput().value(), tmpLocalSize, tmpNumGroups))->copyExtrasFrom(callSite)->setDecorations(add_flag(callSite->decoration, InstructionDecorations::BUILTIN_GLOBAL_SIZE)));
	}
	if(callSite->methodName == "vc4cl_global_id" && callSite->getArguments().size() == 1)
	{
		//global_id(dim) = global_offset(dim) + (group_id(dim) * local_size(dim) + local_id(dim)
		logging::debug() << "Intrinsifying reading of global work-item ids" << logging::endl;

		const Value tmpGroupID = method.addNewLocal(TYPE_INT32, "%group_id");
		const Value tmpLocalSize = method.addNewLocal(TYPE_INT8, "%local_size");
		const Value tmpGlobalOffset = method.addNewLocal(TYPE_INT32, "%global_offset");
		const Value tmpLocalID = method.addNewLocal(TYPE_INT8, "%local_id");
		const Value tmpRes0 = method.addNewLocal(TYPE_INT32, "%global_id");
		const Value tmpRes1 = method.addNewLocal(TYPE_INT32, "%global_id");
		//emplace dummy instructions to be replaced
		it.emplace(new MoveOperation(tmpGroupID, NOP_REGISTER));
		it = intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0).value(), {Method::GROUP_ID_X, Method::GROUP_ID_Y, Method::GROUP_ID_Z}, INT_ZERO, InstructionDecorations::BUILTIN_GROUP_ID);
		it.nextInBlock();
		it.emplace(new MoveOperation(tmpLocalSize, NOP_REGISTER));
		it = intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0).value(), Method::LOCAL_SIZES, InstructionDecorations::BUILTIN_LOCAL_SIZE);
		it.nextInBlock();
		it.emplace(new MoveOperation(tmpGlobalOffset, NOP_REGISTER));
		it = intrinsifyReadWorkGroupInfo(method, it, callSite->getArgument(0).value(), {Method::GLOBAL_OFFSET_X, Method::GLOBAL_OFFSET_Y, Method::GLOBAL_OFFSET_Z}, INT_ZERO, InstructionDecorations::BUILTIN_GLOBAL_OFFSET);
		it.nextInBlock();
		it.emplace(new MoveOperation(tmpLocalID, NOP_REGISTER));
		it = intrinsifyReadWorkItemInfo(method, it, callSite->getArgument(0).value(), Method::LOCAL_IDS, InstructionDecorations::BUILTIN_LOCAL_ID);
		it.nextInBlock();
		it.emplace(new Operation(OP_MUL24, tmpRes0, tmpGroupID, tmpLocalSize));
		it.nextInBlock();
		it.emplace(new Operation(OP_ADD, tmpRes1, tmpGlobalOffset, tmpRes0));
		it.nextInBlock();
		return it.reset((new Operation(OP_ADD, callSite->getOutput().value(), tmpRes1, tmpLocalID))->copyExtrasFrom(callSite)->setDecorations(add_flag(callSite->decoration, InstructionDecorations::BUILTIN_GLOBAL_ID)));
	}
	return it;
}

InstructionWalker optimizations::intrinsify(const Module& module, Method& method, InstructionWalker it, const Configuration& config)
{
	if(!it.has<Operation>() && !it.has<MethodCall>())
		//fail fast
		return it;
	auto newIt = intrinsifyComparison(method, it);
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyWorkItemFunctions(method, it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyNoArgs(method, it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyUnary(method, it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyBinary(method, it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyTernary(method, it);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyArithmetic(method, it, config.mathType);
	}
	if(newIt == it)
	{
		//no changes so far
		newIt = intrinsifyImageFunction(it, method);
	}
	return newIt;
}
