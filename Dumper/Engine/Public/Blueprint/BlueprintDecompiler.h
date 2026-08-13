#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Blueprint/EExprToken.h"

class UEFunction;

class BlueprintDecompiler
{
public:
	enum class OpcodeSemantic : uint8_t
	{
		Unknown = 0,
		ExprToken,
		PrimitiveCast,
	};

	struct OpcodeMapping
	{
		OpcodeSemantic Semantic = OpcodeSemantic::Unknown;
		EExprToken Token = EExprToken::EX_Max;
	};

	struct NameOperandLayout
	{
		uint8_t ByteWidth = 0;
		uint8_t ComparisonIndexOffset = 0;
		uint8_t ComparisonIndexWidth = 0;
		uint8_t NumberOffset = 0;
		uint8_t NumberWidth = 0;
	};

	struct DisassemblyLimits
	{
		size_t MaxInputBytes = 1024 * 1024;
		size_t MaxBytesConsumed = 1024 * 1024;
		size_t MaxInstructions = 100000;
		size_t MaxStringCodeUnits = 65536;
		uint32_t MaxRecursionDepth = 64;
	};

	// A profile is assembled from an externally witnessed engine layout and is
	// passed by const reference. The parser never consults process-global state.
	struct BytecodeProfile
	{
		std::string Id;
		uint8_t PointerWidth = 0;
		uint8_t CodeSkipWidth = 0;
		uint8_t VectorComponentWidth = 0;
		uint8_t RotationComponentWidth = 0;
		uint8_t TransformComponentWidth = 0;
		NameOperandLayout NameLayout;
		DisassemblyLimits Limits;
		std::array<OpcodeMapping, 256> Opcodes{};

		BytecodeProfile& MapOpcode(uint8_t RawOpcode, EExprToken Token);
		BytecodeProfile& MapPrimitiveCast(uint8_t RawOpcode);
	};

	enum class DisassemblyStatus : uint8_t
	{
		Complete = 0,
		Incomplete,
		Error,
	};

	enum class DisassemblyErrorCode : uint8_t
	{
		None = 0,
		ProfileRequired,
		InvalidProfile,
		InputLimitExceeded,
		TotalByteLimitExceeded,
		InstructionLimitExceeded,
		RecursionLimitExceeded,
		StringLimitExceeded,
		TruncatedOperand,
		UnterminatedString,
		UnknownOpcode,
		UnsupportedOpcode,
		UnexpectedTerminator,
		ExpectedTerminator,
		InvalidOperand,
		EndOfScriptMissing,
		TrailingBytes,
	};

	struct DisassemblyError
	{
		bool Present = false;
		size_t Offset = 0;
		DisassemblyErrorCode Code = DisassemblyErrorCode::None;
		std::string Message;
	};

	struct DisassembledInstruction
	{
		size_t Offset = 0;
		size_t Size = 0;
		uint32_t Depth = 0;
		uint8_t RawOpcode = 0;
		OpcodeSemantic Semantic = OpcodeSemantic::Unknown;
		EExprToken Token = EExprToken::EX_Max;
		std::string Text;
	};

	struct DisassemblyResult
	{
		DisassemblyStatus Status = DisassemblyStatus::Error;
		std::string ProfileId;
		std::vector<DisassembledInstruction> Instructions;
		std::string Pseudocode;
		size_t InputSize = 0;
		size_t BytesConsumed = 0;
		double Coverage = 0.0;
		size_t UnknownCount = 0;
		bool SawEndOfScript = false;
		DisassemblyError FirstError;
	};

	struct DecompileResult
	{
		std::string FunctionName;
		std::string ClassName;
		std::string FlagsString;
		int32_t ScriptSize = 0;
		std::string Pseudocode;
	};

	// The only parsing entry point. Object/name operands remain raw tokens, so
	// this method is deterministic and does not dereference live UE objects.
	static DisassemblyResult Disassemble(
		std::span<const uint8_t> Script,
		const BytecodeProfile& Profile);

	// Legacy adapters deliberately do not infer a profile from global offsets,
	// game-version strings, or live name/object arrays.
	static DecompileResult Decompile(const UEFunction& Func);
	static std::string DecompileBytes(const std::vector<uint8_t>& Script);
};
