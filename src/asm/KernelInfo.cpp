/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "KernelInfo.h"

#include "../intermediate/IntermediateInstruction.h"
#include "Instruction.h"
#include "log.h"

#include <array>
#include <cstring>

using namespace vc4c;
using namespace vc4c::qpu_asm;

static void writeStream(std::ostream& stream, const std::array<uint8_t, 8>& buf, const OutputMode mode)
{
    if(mode == OutputMode::BINARY)
    {
        stream.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    }
    else if(mode == OutputMode::HEX)
    {
        const uint64_t binary = *reinterpret_cast<const uint64_t*>(buf.data());
        std::array<char, 64> buffer{};
        snprintf(buffer.data(), buffer.size(), "0x%08x, 0x%08x, ", static_cast<uint32_t>(binary & 0xFFFFFFFFLL), static_cast<uint32_t>((binary & 0xFFFFFFFF00000000LL) >> 32));
        stream << std::string(buffer.data()) << std::endl;
    }
}

static std::size_t copyName(std::ostream& stream, const std::string& name, const OutputMode mode, std::size_t bytesInFirstBlock = 8)
{
	std::array<uint8_t, 8> buf{};
	std::size_t numWords = 0;
    for(std::size_t i = 0; i < name.size(); i += buf.size())
    {
        const std::size_t bytesInBlock = i == 0 ? bytesInFirstBlock : buf.size();
        std::size_t l = std::min(name.size() - i, bytesInBlock);
        //copy name in multiples of 8 byte
        memcpy(buf.data(), name.data() + i, l);
        //pad with zeroes
        memset(buf.data() + l, '\0', bytesInBlock - l);
        writeStream(stream, buf, mode);
        ++numWords;
    }
    return numWords;
}

std::string ParamInfo::to_string() const
{
	//address space
	return std::string((getPointer() && getAddressSpace() == AddressSpace::CONSTANT) ? "__constant " : "") + std::string((getPointer() && getAddressSpace() == AddressSpace::GLOBAL) ? "__global " : "") +
			std::string((getPointer() && getAddressSpace() == AddressSpace::LOCAL) ? "__local " : "") + std::string((getPointer() && getAddressSpace() == AddressSpace::PRIVATE) ? "__private " : "") +
			//access qualifier
			std::string((getPointer() && getConstant()) ? "const " : "") + std::string((getPointer() && getRestricted()) ? "restrict " : "") + std::string((getPointer() && getVolatile()) ? "volatile " : "") +
			//input/output
			((getPointer() && getInput()) ? "in " : "") + ((getPointer() && getOutput()) ? "out " : "") +
			//type + name
			((typeName) + " ") + (name + " (") + (std::to_string(getSize()) + " B, ") + std::to_string(getElements()) + " items)";
}

std::size_t ParamInfo::write(std::ostream& stream, const OutputMode mode) const
{
	std::size_t numWords = 0;
	std::array<uint8_t, 8> buf{};
	if(mode == OutputMode::BINARY || mode == OutputMode::HEX)
	{
		*reinterpret_cast<uint64_t*>(buf.data()) = value;
		writeStream(stream, buf, mode);
		++numWords;
		numWords += copyName(stream, name, mode);
		numWords += copyName(stream, typeName, mode);
	}
	return numWords;
}

KernelInfo::KernelInfo(const std::size_t& numParameters) : Bitfield(0), workGroupSize(0)
{
	parameters.reserve(numParameters);
}

std::size_t KernelInfo::write(std::ostream& stream, const OutputMode mode) const
{
	std::size_t numWords = 0;
    if(mode == OutputMode::HEX || mode == OutputMode::ASSEMBLER)
	{
		const std::string s = to_string();
		stream << "// " << s << std::endl;
	}
    if(mode == OutputMode::BINARY || mode == OutputMode::HEX)
    {
    	std::array<uint8_t, 8> buf{};
        *reinterpret_cast<uint64_t*>(buf.data()) = value;
        writeStream(stream, buf, mode);
        ++numWords;
        *reinterpret_cast<uint64_t*>(buf.data()) = workGroupSize;
        writeStream(stream, buf, mode);
        ++numWords;
        numWords += copyName(stream, name, mode);
        for(const ParamInfo& info : parameters)
        {
            //for each parameter, copy infos and name
        	numWords += info.write(stream, mode);
        }
    }
    return numWords;
}

std::string KernelInfo::to_string() const
{
	return std::string("Kernel '") + (name + "' with ") + (std::to_string(getLength().getValue()) + " instructions, offset ") + (std::to_string(getOffset().getValue()) + ", with following parameters: ") + ::to_string<ParamInfo>(parameters);
}

static void toBinary(const Value& val, std::vector<uint8_t>& queue)
{
	switch(val.valueType)
	{
		case ValueType::CONTAINER:
			for(const Value& element : val.container.elements)
				toBinary(element, queue);
			break;
		case ValueType::LITERAL:
			switch(val.literal.type)
			{
				case LiteralType::BOOL:
					for(std::size_t i = 0; i < val.type.getVectorWidth(true); ++i)
						queue.push_back(static_cast<uint8_t>(val.literal.isTrue()));
					break;
				case LiteralType::INTEGER:
				case LiteralType::REAL:
					for(std::size_t i = 0; i < val.type.getVectorWidth(true); ++i)
					{
						//little endian
						if(val.type.getElementType().getPhysicalWidth() > 3)
							queue.push_back(static_cast<uint8_t>((val.literal.toImmediate() & 0xFF000000) >> 24));
						if(val.type.getElementType().getPhysicalWidth() > 2)
							queue.push_back(static_cast<uint8_t>((val.literal.toImmediate() & 0xFF0000) >> 16));
						if(val.type.getElementType().getPhysicalWidth() > 1)
							queue.push_back(static_cast<uint8_t>((val.literal.toImmediate() & 0xFF00) >> 8));
						queue.push_back(static_cast<uint8_t>(val.literal.toImmediate() & 0xFF));
					}
					break;
				default:
					throw CompilationError(CompilationStep::CODE_GENERATION, "Unrecognized literal-type!");
			}
			break;
		case ValueType::UNDEFINED:
			//e.g. for array <type> undefined, need to reserve enough bytes
			for(std::size_t s = 0; s < val.type.getPhysicalWidth(); ++s)
				queue.push_back(0);
			break;
		default:
			throw CompilationError(CompilationStep::CODE_GENERATION, "Can't map value-type to binary literal!");
	}
}

static std::vector<uint8_t> generateDataSegment(const ReferenceRetainingList<Global>& globalData)
{
	logging::debug() << "Writing data segment for " << globalData.size() << " values..." << logging::endl;
	std::vector<uint8_t> bytes;
	bytes.reserve(2048);
	for(const Global& global : globalData)
	{
		//add alignment per element
		const unsigned alignment = global.type.getPointerType().value()->getAlignment();
		while(bytes.size() % alignment != 0)
		{
			bytes.push_back(0);
		}
		toBinary(global.value, bytes);
	}

	while((bytes.size() % 8) != 0)
	{
		bytes.push_back(0);
	}
	return bytes;
}

std::size_t ModuleInfo::write(std::ostream& stream, const OutputMode mode, const ReferenceRetainingList<Global>& globalData)
{
	std::size_t numWords = 0;
	if(mode == OutputMode::HEX || mode == OutputMode::ASSEMBLER)
	{
		stream << "// Module with " << getInfoCount() << " kernels, global data with " << getGlobalDataSize().getValue() << " words (64-bit each), starting at offset " << getGlobalDataOffset().getValue() << " words and " << getStackFrameSize().getValue() << " words of stack-frame" << std::endl;
	}
	std::array<uint8_t, 8> buf{};
	if(mode== OutputMode::BINARY || mode == OutputMode::HEX)
	{
		//write magic number
		reinterpret_cast<uint32_t*>(buf.data())[0] = QPUASM_MAGIC_NUMBER;
		reinterpret_cast<uint32_t*>(buf.data())[1] = QPUASM_MAGIC_NUMBER;
		writeStream(stream, buf, mode);
		++numWords;

		//write module info
		*reinterpret_cast<uint64_t*>(buf.data()) = value;
		writeStream(stream, buf, mode);
		++numWords;
	}
	//write kernel-infos
	for(const KernelInfo& info : kernelInfos)
	{
		logging::debug() << info.to_string() << logging::endl;
		numWords += info.write(stream, mode);
	}
	//write kernel-info-to-global-data delimiter
	buf.fill(0);
	writeStream(stream, buf, mode);
	++numWords;

	//update global data offset
	setGlobalDataOffset(Word(numWords));

	if(mode == OutputMode::HEX || mode == OutputMode::ASSEMBLER)
	{
		for(const Global& global : globalData)
			stream << "//" << global.to_string(true) << std::endl;
	}

	//write global data, padded to multiples of 8 Byte
	switch (mode)
	{
		case OutputMode::ASSEMBLER:
		{
			for(const Global& global : globalData)
				stream << global.to_string(true) << std::endl;
			break;
		}
		case OutputMode::BINARY:
		{
			const auto binary = generateDataSegment(globalData);
			stream.write(reinterpret_cast<const char*>(binary.data()), binary.size());
			numWords += binary.size() / sizeof(uint64_t);
			break;
		}
		case OutputMode::HEX:
		{
			const auto binary = generateDataSegment(globalData);
			for(const Global& global : globalData)
				stream << "//" << global.to_string(true) << std::endl;
			for(std::size_t i = 0; i < binary.size(); i += 8)
				stream << toHexString((static_cast<uint64_t>(binary.at(i)) << 56) | (static_cast<uint64_t>(binary.at(i+1)) << 48) | (static_cast<uint64_t>(binary.at(i+2)) << 40) |
						(static_cast<uint64_t>(binary.at(i+3)) << 32) | (static_cast<uint64_t>(binary.at(i+4)) << 24) | (static_cast<uint64_t>(binary.at(i+5)) << 16) |
						(static_cast<uint64_t>(binary.at(i+6)) << 8) | static_cast<uint64_t>(binary.at(i+7))) << std::endl;
			numWords += binary.size() / sizeof(uint64_t);
			break;
		}
	}

	//update global data size
	setGlobalDataSize(Word(numWords) - getGlobalDataOffset());

	//write global-data-to-kernel-instructions delimiter
	buf.fill(0);
	writeStream(stream, buf, mode);
	++numWords;

	return numWords;
}

KernelInfo qpu_asm::getKernelInfos(const Method& method, const std::size_t initialOffset, const std::size_t numInstructions)
{
    KernelInfo info(method.parameters.size());
    info.setOffset(Word(initialOffset));
    info.setLength(Word(numInstructions));
    info.setName(method.name[0] == '@' ? method.name.substr(1) : method.name);
    info.workGroupSize = 0;
    {
        size_t requiredSize = 1;
        unsigned offset = 0;
        for(uint32_t size : method.metaData.workGroupSizes)
        {
            info.workGroupSize |= static_cast<uint64_t>(size) << offset;
            offset += 16;
            requiredSize *= size;
        }
        if(requiredSize > KernelInfo::MAX_WORK_GROUP_SIZES)
        {
            logging::error() << "Required work-group size " << requiredSize << " exceeds the limit of " << KernelInfo::MAX_WORK_GROUP_SIZES << logging::endl;
        }
    }
    {
        uint32_t requiredSize = 1;
        for(uint32_t size : method.metaData.workGroupSizeHints)
        {
            requiredSize *= size;
        }
        if(requiredSize > KernelInfo::MAX_WORK_GROUP_SIZES)
        {
            logging::warn() << "Work-group size hint " << requiredSize << " exceeds the limit of " << KernelInfo::MAX_WORK_GROUP_SIZES << logging::endl;
        }
    }
    for(const Parameter& param : method.parameters)
    {
    	std::string paramName = param.parameterName;
    	paramName = paramName.empty() ? param.name : paramName;
        const DataType& paramType = param.type;
        std::string typeName = param.origTypeName;
        ParamInfo paramInfo;
        paramInfo.setSize(static_cast<uint8_t>(paramType.getPhysicalWidth()));
        paramInfo.setPointer(paramType.isPointerType() || paramType.getImageType());
        paramInfo.setOutput(param.isOutputParameter());
        paramInfo.setInput(param.isInputParameter());
        paramInfo.setConstant(has_flag(param.decorations, ParameterDecorations::READ_ONLY));
        paramInfo.setRestricted(has_flag(param.decorations, ParameterDecorations::RESTRICT));
        paramInfo.setVolatile(has_flag(param.decorations, ParameterDecorations::VOLATILE));
        paramInfo.setName(paramName[0] == '%' ? paramName.substr(1) : paramName);
        paramInfo.setElements((paramType.isPointerType() ? static_cast<uint8_t>(1) : paramType.num));
        paramInfo.setAddressSpace(paramType.isPointerType() ? paramType.getPointerType().value()->addressSpace : AddressSpace::PRIVATE);
        paramInfo.setFloatingType(paramType.isFloatingType());
        //FIXME signedness is only recognized correctly for non-32 bit scalar types (e.g. (u)char, (u)short), not for pointers or even vector-types
        paramInfo.setSigned(has_flag(param.decorations, ParameterDecorations::SIGN_EXTEND));
        paramInfo.setUnsigned(has_flag(param.decorations, ParameterDecorations::ZERO_EXTEND));
        paramInfo.setTypeName(typeName.empty() ? paramType.getTypeName(paramInfo.getSigned(), paramInfo.getUnsigned()) : typeName);
        info.addParameter(paramInfo);
    }
    
#ifdef DEBUG_MODE
    if(!method.stackAllocations.empty())
    {
		logging::debug() << "Kernel " << method.name << ":" << logging::endl;
		for(const auto& s : method.stackAllocations)
			logging::debug() << "Stack-Entry: " << s.to_string() << ", size: " << s.size << ", alignment: " << s.alignment << ", offset: " << s.offset << logging::endl;
    }
#endif

    return info;
}
