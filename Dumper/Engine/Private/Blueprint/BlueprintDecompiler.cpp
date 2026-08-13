#include "Blueprint/BlueprintDecompiler.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <string_view>

namespace
{
using ErrorCode = BlueprintDecompiler::DisassemblyErrorCode;
using Instruction = BlueprintDecompiler::DisassembledInstruction;
using OpcodeMapping = BlueprintDecompiler::OpcodeMapping;
using OpcodeSemantic = BlueprintDecompiler::OpcodeSemantic;
using Profile = BlueprintDecompiler::BytecodeProfile;
using Result = BlueprintDecompiler::DisassemblyResult;
using Status = BlueprintDecompiler::DisassemblyStatus;

constexpr std::string_view ProfileRequiredText =
	"// unavailable: BYTECODE_PROFILE_REQUIRED (supply an immutable witnessed BytecodeProfile)\n";

bool IsIntegerWidth(const uint8_t Width)
{
	return Width == 1 || Width == 2 || Width == 4 || Width == 8;
}

bool IsOptionalIntegerWidth(const uint8_t Width)
{
	return Width == 0 || IsIntegerWidth(Width);
}

bool FitsLayoutField(const uint8_t Offset, const uint8_t Width, const uint8_t TotalWidth)
{
	return Width == 0 ||
		(static_cast<size_t>(Offset) + static_cast<size_t>(Width) <= static_cast<size_t>(TotalWidth));
}

std::string BytesToHex(const std::span<const uint8_t> Bytes)
{
	std::string ResultText;
	ResultText.reserve(Bytes.size() * 2);
	for (const uint8_t Byte : Bytes)
		ResultText += std::format("{:02X}", Byte);
	return ResultText;
}

uint64_t DecodeUnsignedLittleEndian(
	const std::span<const uint8_t> Bytes,
	const size_t Offset,
	const size_t Width)
{
	uint64_t Value = 0;
	for (size_t Index = 0; Index < Width; ++Index)
		Value |= static_cast<uint64_t>(Bytes[Offset + Index]) << (Index * 8);
	return Value;
}

void AppendEscapedByte(std::string& Output, const uint8_t Value)
{
	switch (Value)
	{
	case '\\': Output += "\\\\"; return;
	case '"': Output += "\\\""; return;
	case '\n': Output += "\\n"; return;
	case '\r': Output += "\\r"; return;
	case '\t': Output += "\\t"; return;
	default:
		if (Value >= 0x20 && Value <= 0x7E)
			Output.push_back(static_cast<char>(Value));
		else
			Output += std::format("\\x{:02X}", Value);
		return;
	}
}

void AppendEscapedUtf16(std::string& Output, const uint16_t Value)
{
	if (Value <= 0x7F)
	{
		AppendEscapedByte(Output, static_cast<uint8_t>(Value));
		return;
	}
	Output += std::format("\\u{:04X}", Value);
}

std::string RawPointerToken(const std::string_view Kind, const uint64_t Value)
{
	return std::format("{}Token(0x{:X})", Kind, Value);
}

bool ValidateProfile(const Profile& ProfileValue, std::string& Error)
{
	if (ProfileValue.Id.empty())
	{
		Error = "profile id is empty";
		return false;
	}
	if (ProfileValue.PointerWidth != 4 && ProfileValue.PointerWidth != 8)
	{
		Error = "pointer width must be exactly 4 or 8 bytes";
		return false;
	}
	if (!IsIntegerWidth(ProfileValue.CodeSkipWidth))
	{
		Error = "code-skip width must be 1, 2, 4, or 8 bytes";
		return false;
	}
	if ((ProfileValue.VectorComponentWidth != 4 && ProfileValue.VectorComponentWidth != 8) ||
		(ProfileValue.RotationComponentWidth != 4 && ProfileValue.RotationComponentWidth != 8) ||
		(ProfileValue.TransformComponentWidth != 4 && ProfileValue.TransformComponentWidth != 8))
	{
		Error = "vector, rotation, and transform component widths must each be exactly 4 or 8 bytes";
		return false;
	}
	const auto& Name = ProfileValue.NameLayout;
	if (Name.ByteWidth == 0 || Name.ByteWidth > 32)
	{
		Error = "name operand width must be between 1 and 32 bytes";
		return false;
	}
	if (!IsOptionalIntegerWidth(Name.ComparisonIndexWidth) ||
		!IsOptionalIntegerWidth(Name.NumberWidth) ||
		!FitsLayoutField(Name.ComparisonIndexOffset, Name.ComparisonIndexWidth, Name.ByteWidth) ||
		!FitsLayoutField(Name.NumberOffset, Name.NumberWidth, Name.ByteWidth))
	{
		Error = "name operand fields do not fit the declared layout";
		return false;
	}
	const auto& Limits = ProfileValue.Limits;
	if (Limits.MaxInputBytes == 0 || Limits.MaxBytesConsumed == 0 ||
		Limits.MaxInstructions == 0 || Limits.MaxStringCodeUnits == 0 ||
		Limits.MaxRecursionDepth == 0)
	{
		Error = "all disassembly limits must be non-zero";
		return false;
	}

	bool HasEndOfScript = false;
	for (const OpcodeMapping& Mapping : ProfileValue.Opcodes)
	{
		if (Mapping.Semantic == OpcodeSemantic::ExprToken)
		{
			if (Mapping.Token == EExprToken::EX_Max)
			{
				Error = "mapped ExprToken cannot use EX_Max";
				return false;
			}
			HasEndOfScript = HasEndOfScript || Mapping.Token == EExprToken::EX_EndOfScript;
		}
		else if (Mapping.Semantic == OpcodeSemantic::PrimitiveCast &&
			Mapping.Token != EExprToken::EX_Max)
		{
			Error = "PrimitiveCast mapping must not carry an ExprToken";
			return false;
		}
	}
	if (!HasEndOfScript)
	{
		Error = "profile has no EndOfScript opcode mapping";
		return false;
	}
	return true;
}

class BytecodeReader
{
public:
	BytecodeReader(const std::span<const uint8_t> ScriptValue, const size_t MaxBytesValue)
		: Script(ScriptValue),
		  MaxBytes(MaxBytesValue < ScriptValue.size() ? MaxBytesValue : ScriptValue.size())
	{
	}

	[[nodiscard]] size_t Position() const { return Current; }
	[[nodiscard]] bool AtEnd() const { return Current >= Script.size(); }
	[[nodiscard]] bool Failed() const { return Error.Present; }
	[[nodiscard]] const BlueprintDecompiler::DisassemblyError& FirstError() const { return Error; }

	bool Fail(const ErrorCode Code, const size_t Offset, std::string Message)
	{
		if (!Error.Present)
		{
			Error.Present = true;
			Error.Offset = Offset;
			Error.Code = Code;
			Error.Message = std::move(Message);
		}
		return false;
	}

	bool TryPeekByte(uint8_t& Value)
	{
		if (!CanRead(1, "opcode"))
			return false;
		Value = Script[Current];
		return true;
	}

	bool TryReadByte(uint8_t& Value)
	{
		return TryReadScalar(Value, "byte operand");
	}

	bool TryReadUInt16(uint16_t& Value)
	{
		return TryReadScalar(Value, "uint16 operand");
	}

	bool TryReadInt32(int32_t& Value)
	{
		return TryReadScalar(Value, "int32 operand");
	}

	bool TryReadInt64(int64_t& Value)
	{
		return TryReadScalar(Value, "int64 operand");
	}

	bool TryReadUInt64(uint64_t& Value)
	{
		return TryReadScalar(Value, "uint64 operand");
	}

	bool TryReadFloat(float& Value)
	{
		return TryReadScalar(Value, "float operand");
	}

	bool TryReadDouble(double& Value)
	{
		return TryReadScalar(Value, "double operand");
	}

	bool TryReadUnsigned(const uint8_t Width, uint64_t& Value, const std::string_view Label)
	{
		if (!CanRead(Width, Label))
			return false;
		Value = DecodeUnsignedLittleEndian(Script, Current, Width);
		Current += Width;
		return true;
	}

	bool TryReadBytes(const size_t Count, std::span<const uint8_t>& Value, const std::string_view Label)
	{
		if (!CanRead(Count, Label))
			return false;
		Value = Script.subspan(Current, Count);
		Current += Count;
		return true;
	}

	bool TryReadAnsiString(const size_t MaxCodeUnits, std::string& Value)
	{
		const size_t Start = Current;
		Value.clear();
		for (size_t Count = 0; Count < MaxCodeUnits; ++Count)
		{
			if (AtEnd())
				return Fail(ErrorCode::UnterminatedString, Start, "ANSI string is not null-terminated");
			uint8_t CodeUnit = 0;
			if (!TryReadByte(CodeUnit))
				return false;
			if (CodeUnit == 0)
				return true;
			AppendEscapedByte(Value, CodeUnit);
		}
		return Fail(ErrorCode::StringLimitExceeded, Start, "ANSI string exceeds profile limit");
	}

	bool TryReadUtf16String(const size_t MaxCodeUnits, std::string& Value)
	{
		const size_t Start = Current;
		Value.clear();
		for (size_t Count = 0; Count < MaxCodeUnits; ++Count)
		{
			if (AtEnd())
				return Fail(ErrorCode::UnterminatedString, Start, "UTF-16 string is not null-terminated");
			uint16_t CodeUnit = 0;
			if (!TryReadUInt16(CodeUnit))
				return false;
			if (CodeUnit == 0)
				return true;
			AppendEscapedUtf16(Value, CodeUnit);
		}
		return Fail(ErrorCode::StringLimitExceeded, Start, "UTF-16 string exceeds profile limit");
	}

private:
	template <typename T>
	bool TryReadScalar(T& Value, const std::string_view Label)
	{
		if (!CanRead(sizeof(T), Label))
			return false;
		std::memcpy(&Value, Script.data() + Current, sizeof(T));
		Current += sizeof(T);
		return true;
	}

	bool CanRead(const size_t Count, const std::string_view Label)
	{
		if (Failed())
			return false;
		if (Current > MaxBytes || Count > MaxBytes - Current)
		{
			if (Current <= Script.size() && Count <= Script.size() - Current)
				return Fail(ErrorCode::TotalByteLimitExceeded, Current,
					std::format("{} exceeds total byte limit", Label));
			return Fail(ErrorCode::TruncatedOperand, Current,
				std::format("{} is truncated", Label));
		}
		if (Current > Script.size() || Count > Script.size() - Current)
			return Fail(ErrorCode::TruncatedOperand, Current,
				std::format("{} is truncated", Label));
		return true;
	}

	std::span<const uint8_t> Script;
	size_t MaxBytes = 0;
	size_t Current = 0;
	BlueprintDecompiler::DisassemblyError Error;
};

class BoundedParser
{
public:
	BoundedParser(const std::span<const uint8_t> ScriptValue, const Profile& ProfileValue)
		: Script(ScriptValue), ProfileData(ProfileValue), Reader(ScriptValue, ProfileValue.Limits.MaxBytesConsumed)
	{
		Output.ProfileId = ProfileValue.Id;
		Output.InputSize = ScriptValue.size();
	}

	Result Run()
	{
		if (Script.empty())
		{
			Reader.Fail(ErrorCode::EndOfScriptMissing, 0, "script is empty and has no EndOfScript opcode");
			return Finish(Status::Incomplete);
		}

		while (!Reader.Failed())
		{
			if (Reader.AtEnd())
			{
				Reader.Fail(ErrorCode::EndOfScriptMissing, Reader.Position(),
					"script ended without EndOfScript opcode");
				break;
			}

			uint8_t RawOpcode = 0;
			if (!Reader.TryPeekByte(RawOpcode))
				break;
			const OpcodeMapping Mapping = ProfileData.Opcodes[RawOpcode];
			if (Mapping.Semantic == OpcodeSemantic::ExprToken &&
				Mapping.Token == EExprToken::EX_EndOfScript)
			{
				std::string Ignored;
				if (!ParseExpression(0, Ignored))
					break;
				Output.SawEndOfScript = true;
				if (!Reader.AtEnd())
				{
					Reader.Fail(ErrorCode::TrailingBytes, Reader.Position(),
						"bytes remain after EndOfScript opcode");
					break;
				}
				return Finish(Status::Complete);
			}

			const size_t Offset = Reader.Position();
			std::string Expression;
			if (!ParseExpression(0, Expression))
				break;
			if (!Expression.empty())
				Output.Pseudocode += std::format("  {:04X}: {}\n", Offset, Expression);
		}

		return Finish(Status::Incomplete);
	}

private:
	Result Finish(const Status RequestedStatus)
	{
		Output.BytesConsumed = Reader.Position();
		const double RawCoverage = Script.empty()
			? 0.0
			: static_cast<double>(Output.BytesConsumed) / static_cast<double>(Script.size());
		Output.Coverage = RawCoverage < 1.0 ? RawCoverage : 1.0;
		Output.FirstError = Reader.FirstError();
		Output.Status = Reader.Failed() && RequestedStatus == Status::Complete
			? Status::Incomplete
			: RequestedStatus;
		return std::move(Output);
	}

	bool CheckDepth(const uint32_t Depth)
	{
		if (Depth <= ProfileData.Limits.MaxRecursionDepth)
			return true;
		return Reader.Fail(ErrorCode::RecursionLimitExceeded, Reader.Position(),
			"expression recursion exceeds profile limit");
	}

	bool BeginInstruction(
		const uint32_t Depth,
		size_t& InstructionIndex,
		OpcodeMapping& Mapping)
	{
		if (!CheckDepth(Depth))
			return false;
		if (Output.Instructions.size() >= ProfileData.Limits.MaxInstructions)
			return Reader.Fail(ErrorCode::InstructionLimitExceeded, Reader.Position(),
				"instruction count exceeds profile limit");

		const size_t Offset = Reader.Position();
		uint8_t RawOpcode = 0;
		if (!Reader.TryReadByte(RawOpcode))
			return false;

		Mapping = ProfileData.Opcodes[RawOpcode];
		Instruction Entry;
		Entry.Offset = Offset;
		Entry.Size = 1;
		Entry.Depth = Depth;
		Entry.RawOpcode = RawOpcode;
		Entry.Semantic = Mapping.Semantic;
		Entry.Token = Mapping.Token;
		InstructionIndex = Output.Instructions.size();
		Output.Instructions.push_back(std::move(Entry));

		if (Mapping.Semantic == OpcodeSemantic::Unknown)
		{
			++Output.UnknownCount;
			Output.Instructions[InstructionIndex].Text =
				std::format("unknown_opcode(0x{:02X})", RawOpcode);
			return Reader.Fail(ErrorCode::UnknownOpcode, Offset,
				std::format("opcode 0x{:02X} is not mapped by profile {}", RawOpcode, ProfileData.Id));
		}
		return true;
	}

	void CompleteInstruction(const size_t InstructionIndex, std::string Text)
	{
		Instruction& Entry = Output.Instructions[InstructionIndex];
		Entry.Size = Reader.Position() - Entry.Offset;
		Entry.Text = std::move(Text);
	}

	void PreservePartialInstruction(const size_t InstructionIndex)
	{
		Instruction& Entry = Output.Instructions[InstructionIndex];
		Entry.Size = Reader.Position() - Entry.Offset;
		if (Entry.Text.empty())
			Entry.Text = "incomplete_instruction";
	}

	bool PeekToken(EExprToken& Token, uint8_t& RawOpcode)
	{
		if (!Reader.TryPeekByte(RawOpcode))
			return false;
		const OpcodeMapping Mapping = ProfileData.Opcodes[RawOpcode];
		if (Mapping.Semantic != OpcodeSemantic::ExprToken)
		{
			Token = EExprToken::EX_Max;
			return true;
		}
		Token = Mapping.Token;
		return true;
	}

	bool ConsumeExpectedToken(const EExprToken Expected, const uint32_t Depth)
	{
		EExprToken Actual = EExprToken::EX_Max;
		uint8_t RawOpcode = 0;
		if (!PeekToken(Actual, RawOpcode))
			return false;
		if (Actual != Expected)
			return Reader.Fail(ErrorCode::ExpectedTerminator, Reader.Position(),
				std::format("expected {} terminator, found opcode 0x{:02X}",
					GetExprTokenName(Expected), RawOpcode));

		size_t InstructionIndex = 0;
		OpcodeMapping Mapping;
		if (!BeginInstruction(Depth, InstructionIndex, Mapping))
			return false;
		if (Mapping.Semantic != OpcodeSemantic::ExprToken || Mapping.Token != Expected)
		{
			PreservePartialInstruction(InstructionIndex);
			return Reader.Fail(ErrorCode::ExpectedTerminator, Reader.Position() - 1,
				"opcode mapping changed while consuming a terminator");
		}
		CompleteInstruction(InstructionIndex, GetExprTokenName(Expected));
		return true;
	}

	bool ReadPointerToken(const std::string_view Kind, std::string& Text)
	{
		uint64_t Value = 0;
		if (!Reader.TryReadUnsigned(ProfileData.PointerWidth, Value, "pointer operand"))
			return false;
		Text = RawPointerToken(Kind, Value);
		return true;
	}

	bool ReadNameToken(std::string& Text)
	{
		std::span<const uint8_t> Bytes;
		if (!Reader.TryReadBytes(ProfileData.NameLayout.ByteWidth, Bytes, "name operand"))
			return false;

		Text = std::format("NameToken(raw=0x{}", BytesToHex(Bytes));
		const auto& Layout = ProfileData.NameLayout;
		if (Layout.ComparisonIndexWidth != 0)
		{
			const uint64_t Value = DecodeUnsignedLittleEndian(
				Bytes, Layout.ComparisonIndexOffset, Layout.ComparisonIndexWidth);
			Text += std::format(", comparison_index={}", Value);
		}
		if (Layout.NumberWidth != 0)
		{
			const uint64_t Value = DecodeUnsignedLittleEndian(
				Bytes, Layout.NumberOffset, Layout.NumberWidth);
			Text += std::format(", number={}", Value);
		}
		Text += ")";
		return true;
	}

	bool ReadCodeOffset(uint64_t& Value)
	{
		return Reader.TryReadUnsigned(ProfileData.CodeSkipWidth, Value, "code offset operand");
	}

	bool ReadReal(const uint8_t Width, double& Value, const std::string_view Label)
	{
		if (Width == 4)
		{
			float NarrowValue = 0.0F;
			if (!Reader.TryReadFloat(NarrowValue))
				return false;
			Value = static_cast<double>(NarrowValue);
			return true;
		}
		if (Width == 8)
			return Reader.TryReadDouble(Value);
		return Reader.Fail(ErrorCode::InvalidProfile, Reader.Position(),
			std::format("{} has an invalid component width", Label));
	}

	bool ParseCallArguments(const uint32_t Depth, std::string& Arguments)
	{
		Arguments.clear();
		bool First = true;
		while (!Reader.Failed())
		{
			if (Reader.AtEnd())
				return Reader.Fail(ErrorCode::ExpectedTerminator, Reader.Position(),
					"function arguments ended without EndFunctionParms");

			EExprToken Token = EExprToken::EX_Max;
			uint8_t RawOpcode = 0;
			if (!PeekToken(Token, RawOpcode))
				return false;
			if (Token == EExprToken::EX_EndFunctionParms)
				return ConsumeExpectedToken(EExprToken::EX_EndFunctionParms, Depth + 1);

			std::string Argument;
			if (!ParseExpression(Depth + 1, Argument))
				return false;
			if (!First)
				Arguments += ", ";
			First = false;
			Arguments += Argument;
		}
		return false;
	}

	bool ParseUntilTerminator(
		const EExprToken Terminator,
		const uint32_t Depth,
		std::string& Elements,
		const bool ParsePairs)
	{
		Elements.clear();
		bool First = true;
		while (!Reader.Failed())
		{
			if (Reader.AtEnd())
				return Reader.Fail(ErrorCode::ExpectedTerminator, Reader.Position(),
					std::format("container ended without {}", GetExprTokenName(Terminator)));
			EExprToken Token = EExprToken::EX_Max;
			uint8_t RawOpcode = 0;
			if (!PeekToken(Token, RawOpcode))
				return false;
			if (Token == Terminator)
				return ConsumeExpectedToken(Terminator, Depth + 1);

			std::string FirstValue;
			if (!ParseExpression(Depth + 1, FirstValue))
				return false;
			std::string Element = FirstValue;
			if (ParsePairs)
			{
				std::string SecondValue;
				if (!ParseExpression(Depth + 1, SecondValue))
					return false;
				Element += ": " + SecondValue;
			}
			if (!First)
				Elements += ", ";
			First = false;
			Elements += Element;
		}
		return false;
	}

	bool ParseCountedElements(
		const int32_t Count,
		const bool ParsePairs,
		const EExprToken Terminator,
		const uint32_t Depth,
		std::string& Elements)
	{
		if (Count < 0 || static_cast<size_t>(Count) > ProfileData.Limits.MaxInstructions)
			return Reader.Fail(ErrorCode::InvalidOperand, Reader.Position(),
				"container element count is outside the profile bounds");
		Elements.clear();
		for (int32_t Index = 0; Index < Count; ++Index)
		{
			std::string FirstValue;
			if (!ParseExpression(Depth + 1, FirstValue))
				return false;
			std::string Element = FirstValue;
			if (ParsePairs)
			{
				std::string SecondValue;
				if (!ParseExpression(Depth + 1, SecondValue))
					return false;
				Element += ": " + SecondValue;
			}
			if (Index != 0)
				Elements += ", ";
			Elements += Element;
		}
		return ConsumeExpectedToken(Terminator, Depth + 1);
	}

	bool ParseExpression(const uint32_t Depth, std::string& Text)
	{
		size_t InstructionIndex = 0;
		OpcodeMapping Mapping;
		if (!BeginInstruction(Depth, InstructionIndex, Mapping))
			return false;

		bool Success = false;
		if (Mapping.Semantic == OpcodeSemantic::PrimitiveCast)
		{
			uint8_t CastKind = 0;
			std::string Value;
			Success = Reader.TryReadByte(CastKind) && ParseExpression(Depth + 1, Value);
			if (Success)
				Text = std::format("PrimitiveCast(0x{:02X}, {})", CastKind, Value);
		}
		else
		{
			Success = ParseExprToken(Mapping.Token, Depth, Text);
		}

		if (Success)
			CompleteInstruction(InstructionIndex, Text);
		else
			PreservePartialInstruction(InstructionIndex);
		return Success;
	}

	bool ParseExprToken(const EExprToken Token, const uint32_t Depth, std::string& Text)
	{
		switch (Token)
		{
		case EExprToken::EX_IntConst:
		{
			int32_t Value = 0;
			if (!Reader.TryReadInt32(Value)) return false;
			Text = std::to_string(Value);
			return true;
		}
		case EExprToken::EX_FloatConst:
		{
			float Value = 0.0F;
			if (!Reader.TryReadFloat(Value)) return false;
			Text = std::format("{:.4f}f", Value);
			return true;
		}
		case EExprToken::EX_DoubleConst:
		{
			double Value = 0.0;
			if (!Reader.TryReadDouble(Value)) return false;
			Text = std::format("{:.6f}", Value);
			return true;
		}
		case EExprToken::EX_StringConst:
		{
			std::string Value;
			if (!Reader.TryReadAnsiString(ProfileData.Limits.MaxStringCodeUnits, Value)) return false;
			Text = std::format("\"{}\"", Value);
			return true;
		}
		case EExprToken::EX_UnicodeStringConst:
		{
			std::string Value;
			if (!Reader.TryReadUtf16String(ProfileData.Limits.MaxStringCodeUnits, Value)) return false;
			Text = std::format("L\"{}\"", Value);
			return true;
		}
		case EExprToken::EX_ByteConst:
		case EExprToken::EX_IntConstByte:
		{
			uint8_t Value = 0;
			if (!Reader.TryReadByte(Value)) return false;
			Text = std::to_string(Value);
			return true;
		}
		case EExprToken::EX_Int64Const:
		{
			int64_t Value = 0;
			if (!Reader.TryReadInt64(Value)) return false;
			Text = std::to_string(Value) + "LL";
			return true;
		}
		case EExprToken::EX_UInt64Const:
		{
			uint64_t Value = 0;
			if (!Reader.TryReadUInt64(Value)) return false;
			Text = std::to_string(Value) + "ULL";
			return true;
		}
		case EExprToken::EX_IntZero: Text = "0"; return true;
		case EExprToken::EX_IntOne: Text = "1"; return true;
		case EExprToken::EX_True: Text = "true"; return true;
		case EExprToken::EX_False: Text = "false"; return true;
		case EExprToken::EX_NoObject:
		case EExprToken::EX_NoInterface: Text = "nullptr"; return true;
		case EExprToken::EX_Self: Text = "this"; return true;
		case EExprToken::EX_Nothing: Text.clear(); return true;

		case EExprToken::EX_LocalVariable:
		case EExprToken::EX_LocalOutVariable:
		case EExprToken::EX_InstanceVariable:
		case EExprToken::EX_DefaultVariable:
		case EExprToken::EX_ObjectConst:
		case EExprToken::EX_PropertyConst:
		case EExprToken::EX_ClassSparseDataVariable:
		{
			std::string Value;
			const std::string_view Kind = Token == EExprToken::EX_ObjectConst
				? "Object"
				: (Token == EExprToken::EX_ClassSparseDataVariable ? "SparseProperty" : "Property");
			if (!ReadPointerToken(Kind, Value)) return false;
			Text = Value;
			return true;
		}
		case EExprToken::EX_NameConst:
		{
			std::string Value;
			if (!ReadNameToken(Value)) return false;
			Text = Value;
			return true;
		}
		case EExprToken::EX_SoftObjectConst:
		case EExprToken::EX_FieldPathConst:
		{
			std::string Value;
			if (!ParseExpression(Depth + 1, Value)) return false;
			Text = std::format("{}({})",
				Token == EExprToken::EX_SoftObjectConst ? "SoftObject" : "FieldPath", Value);
			return true;
		}

		case EExprToken::EX_FinalFunction:
		case EExprToken::EX_LocalFinalFunction:
		case EExprToken::EX_CallMath:
		case EExprToken::EX_CallMulticastDelegate:
		{
			std::string Function;
			std::string Arguments;
			if (!ReadPointerToken("Function", Function) || !ParseCallArguments(Depth, Arguments)) return false;
			if (Token == EExprToken::EX_CallMath)
				Text = std::format("Math::{}({})", Function, Arguments);
			else if (Token == EExprToken::EX_CallMulticastDelegate)
				Text = std::format("{}.Broadcast({})", Function, Arguments);
			else
				Text = std::format("{}({})", Function, Arguments);
			return true;
		}
		case EExprToken::EX_VirtualFunction:
		case EExprToken::EX_LocalVirtualFunction:
		{
			std::string Function;
			std::string Arguments;
			if (!ReadNameToken(Function) || !ParseCallArguments(Depth, Arguments)) return false;
			Text = std::format("{}({})", Function, Arguments);
			return true;
		}

		case EExprToken::EX_Let:
		{
			std::string Property;
			std::string Variable;
			std::string Value;
			if (!ReadPointerToken("Property", Property) ||
				!ParseExpression(Depth + 1, Variable) ||
				!ParseExpression(Depth + 1, Value)) return false;
			Text = std::format("{} = {} /* {} */", Variable, Value, Property);
			return true;
		}
		case EExprToken::EX_LetBool:
		case EExprToken::EX_LetObj:
		case EExprToken::EX_LetWeakObjPtr:
		case EExprToken::EX_LetDelegate:
		case EExprToken::EX_LetMulticastDelegate:
		{
			std::string Variable;
			std::string Value;
			if (!ParseExpression(Depth + 1, Variable) || !ParseExpression(Depth + 1, Value)) return false;
			Text = std::format("{} = {}", Variable, Value);
			return true;
		}
		case EExprToken::EX_LetValueOnPersistentFrame:
		{
			std::string Property;
			std::string Value;
			if (!ReadPointerToken("Property", Property) || !ParseExpression(Depth + 1, Value)) return false;
			Text = std::format("PersistentFrame[{}] = {}", Property, Value);
			return true;
		}

		case EExprToken::EX_Jump:
		case EExprToken::EX_PushExecutionFlow:
		case EExprToken::EX_SkipOffsetConst:
		{
			uint64_t Offset = 0;
			if (!ReadCodeOffset(Offset)) return false;
			if (Token == EExprToken::EX_Jump)
				Text = std::format("goto 0x{:X}", Offset);
			else if (Token == EExprToken::EX_PushExecutionFlow)
				Text = std::format("push_flow(0x{:X})", Offset);
			else
				Text = std::format("skip_offset(0x{:X})", Offset);
			return true;
		}
		case EExprToken::EX_JumpIfNot:
		{
			uint64_t Offset = 0;
			std::string Condition;
			if (!ReadCodeOffset(Offset) || !ParseExpression(Depth + 1, Condition)) return false;
			Text = std::format("if (!{}) goto 0x{:X}", Condition, Offset);
			return true;
		}
		case EExprToken::EX_Return:
		{
			std::string Value;
			if (!ParseExpression(Depth + 1, Value)) return false;
			Text = Value.empty() ? "return" : "return " + Value;
			return true;
		}
		case EExprToken::EX_PopExecutionFlow: Text = "pop_flow()"; return true;
		case EExprToken::EX_PopExecutionFlowIfNot:
		case EExprToken::EX_ComputedJump:
		{
			std::string Value;
			if (!ParseExpression(Depth + 1, Value)) return false;
			Text = Token == EExprToken::EX_ComputedJump
				? std::format("goto [{}]", Value)
				: std::format("pop_flow_if_not({})", Value);
			return true;
		}

		case EExprToken::EX_Context:
		case EExprToken::EX_Context_FailSilent:
		case EExprToken::EX_ClassContext:
		{
			std::string Object;
			std::string Property;
			std::string Member;
			uint64_t SkipOffset = 0;
			if (!ParseExpression(Depth + 1, Object) || !ReadCodeOffset(SkipOffset) ||
				!ReadPointerToken("Property", Property) || !ParseExpression(Depth + 1, Member)) return false;
			const char* Separator = Token == EExprToken::EX_ClassContext ? "::" : ".";
			Text = std::format("{}{}{} /* skip=0x{:X}, {} */", Object, Separator, Member, SkipOffset, Property);
			return true;
		}
		case EExprToken::EX_InterfaceContext:
		{
			return ParseExpression(Depth + 1, Text);
		}

		case EExprToken::EX_DynamicCast:
		case EExprToken::EX_ObjToInterfaceCast:
		case EExprToken::EX_CrossInterfaceCast:
		case EExprToken::EX_InterfaceToObjCast:
		case EExprToken::EX_MetaCast:
		{
			std::string Class;
			std::string Value;
			if (!ReadPointerToken("Class", Class) || !ParseExpression(Depth + 1, Value)) return false;
			Text = std::format("Cast<{}>({})", Class, Value);
			return true;
		}

		case EExprToken::EX_VectorConst:
		case EExprToken::EX_RotationConst:
		{
			double A = 0.0;
			double B = 0.0;
			double C = 0.0;
			const uint8_t ComponentWidth = Token == EExprToken::EX_VectorConst
				? ProfileData.VectorComponentWidth : ProfileData.RotationComponentWidth;
			if (!ReadReal(ComponentWidth, A, "vector/rotation constant") ||
				!ReadReal(ComponentWidth, B, "vector/rotation constant") ||
				!ReadReal(ComponentWidth, C, "vector/rotation constant")) return false;
			Text = std::format("{}({:.2f}, {:.2f}, {:.2f})",
				Token == EExprToken::EX_VectorConst ? "FVector" : "FRotator", A, B, C);
			return true;
		}
		case EExprToken::EX_TransformConst:
		{
			std::span<const uint8_t> RawTransform;
			const size_t ByteWidth = static_cast<size_t>(ProfileData.TransformComponentWidth) * 10;
			if (!Reader.TryReadBytes(ByteWidth, RawTransform, "transform constant")) return false;
			Text = std::format("TransformToken(raw=0x{})", BytesToHex(RawTransform));
			return true;
		}
		case EExprToken::EX_StructConst:
		{
			std::string Struct;
			int32_t StructSize = 0;
			std::string Fields;
			if (!ReadPointerToken("Struct", Struct) || !Reader.TryReadInt32(StructSize) ||
				!ParseUntilTerminator(EExprToken::EX_EndStructConst, Depth, Fields, false)) return false;
			Text = std::format("{}{{ {} }} /* declared_size={} */", Struct, Fields, StructSize);
			return true;
		}

		case EExprToken::EX_InstanceDelegate:
		{
			std::string Function;
			if (!ReadNameToken(Function)) return false;
			Text = std::format("Delegate({})", Function);
			return true;
		}
		case EExprToken::EX_BindDelegate:
		{
			std::string Function;
			std::string Delegate;
			std::string Object;
			if (!ReadNameToken(Function) || !ParseExpression(Depth + 1, Delegate) ||
				!ParseExpression(Depth + 1, Object)) return false;
			Text = std::format("BindDelegate({}, {}, {})", Delegate, Function, Object);
			return true;
		}
		case EExprToken::EX_AddMulticastDelegate:
		case EExprToken::EX_RemoveMulticastDelegate:
		{
			std::string Delegate;
			std::string Function;
			if (!ParseExpression(Depth + 1, Delegate) || !ParseExpression(Depth + 1, Function)) return false;
			Text = std::format("{}.{}({})", Delegate,
				Token == EExprToken::EX_AddMulticastDelegate ? "Add" : "Remove", Function);
			return true;
		}
		case EExprToken::EX_ClearMulticastDelegate:
		{
			std::string Delegate;
			if (!ParseExpression(Depth + 1, Delegate)) return false;
			Text = std::format("{}.Clear()", Delegate);
			return true;
		}

		case EExprToken::EX_Skip:
		{
			uint64_t Skip = 0;
			if (!ReadCodeOffset(Skip) || !ParseExpression(Depth + 1, Text)) return false;
			Text += std::format(" /* skip=0x{:X} */", Skip);
			return true;
		}
		case EExprToken::EX_Assert:
		{
			uint16_t Line = 0;
			uint8_t DebugMode = 0;
			std::string Value;
			if (!Reader.TryReadUInt16(Line) || !Reader.TryReadByte(DebugMode) ||
				!ParseExpression(Depth + 1, Value)) return false;
			Text = std::format("assert({}) /* line={}, debug={} */", Value, Line, DebugMode);
			return true;
		}
		case EExprToken::EX_Breakpoint: Text = "breakpoint()"; return true;
		case EExprToken::EX_WireTracepoint: Text = "wire_tracepoint()"; return true;
		case EExprToken::EX_Tracepoint: Text = "tracepoint()"; return true;
		case EExprToken::EX_DeprecatedOp4A: Text = "deprecated_opcode()"; return true;
		case EExprToken::EX_InstrumentationEvent:
		{
			uint8_t EventType = 0;
			if (!Reader.TryReadByte(EventType)) return false;
			if (EventType == 4)
			{
				std::string InlineName;
				if (!ReadNameToken(InlineName)) return false;
				Text = std::format("instrumentation_event({}, {})", EventType, InlineName);
			}
			else
			{
				Text = std::format("instrumentation_event({})", EventType);
			}
			return true;
		}

		case EExprToken::EX_SetArray:
		{
			std::string Array;
			std::string Elements;
			if (!ParseExpression(Depth + 1, Array) ||
				!ParseUntilTerminator(EExprToken::EX_EndArray, Depth, Elements, false)) return false;
			Text = std::format("{} = [{}]", Array, Elements);
			return true;
		}
		case EExprToken::EX_ArrayGetByRef:
		{
			std::string Array;
			std::string Index;
			if (!ParseExpression(Depth + 1, Array) || !ParseExpression(Depth + 1, Index)) return false;
			Text = std::format("{}[{}]", Array, Index);
			return true;
		}
		case EExprToken::EX_ArrayConst:
		case EExprToken::EX_SetConst:
		{
			std::string InnerProperty;
			int32_t Count = 0;
			std::string Elements;
			if (!ReadPointerToken("Property", InnerProperty) || !Reader.TryReadInt32(Count)) return false;
			const EExprToken Terminator = Token == EExprToken::EX_ArrayConst
				? EExprToken::EX_EndArrayConst : EExprToken::EX_EndSetConst;
			if (!ParseCountedElements(Count, false, Terminator, Depth, Elements)) return false;
			Text = std::format("{}{{ {} }} /* {} */",
				Token == EExprToken::EX_ArrayConst ? "TArray" : "TSet", Elements, InnerProperty);
			return true;
		}
		case EExprToken::EX_SetSet:
		case EExprToken::EX_SetMap:
		{
			std::string Container;
			int32_t Count = 0;
			std::string Elements;
			if (!ParseExpression(Depth + 1, Container) || !Reader.TryReadInt32(Count)) return false;
			const bool Pairs = Token == EExprToken::EX_SetMap;
			const EExprToken Terminator = Pairs ? EExprToken::EX_EndMap : EExprToken::EX_EndSet;
			if (!ParseCountedElements(Count, Pairs, Terminator, Depth, Elements)) return false;
			Text = std::format("{} = {}{{ {} }}", Container, Pairs ? "TMap" : "TSet", Elements);
			return true;
		}
		case EExprToken::EX_MapConst:
		{
			std::string KeyProperty;
			std::string ValueProperty;
			int32_t Count = 0;
			std::string Elements;
			if (!ReadPointerToken("Property", KeyProperty) || !ReadPointerToken("Property", ValueProperty) ||
				!Reader.TryReadInt32(Count) ||
				!ParseCountedElements(Count, true, EExprToken::EX_EndMapConst, Depth, Elements)) return false;
			Text = std::format("TMap{{ {} }} /* key={}, value={} */", Elements, KeyProperty, ValueProperty);
			return true;
		}

		case EExprToken::EX_SwitchValue:
		{
			uint16_t Count = 0;
			uint64_t EndOffset = 0;
			std::string Index;
			if (!Reader.TryReadUInt16(Count) || !ReadCodeOffset(EndOffset) ||
				!ParseExpression(Depth + 1, Index)) return false;
			if (static_cast<size_t>(Count) > ProfileData.Limits.MaxInstructions)
				return Reader.Fail(ErrorCode::InvalidOperand, Reader.Position(),
					"switch case count is outside the profile bounds");
			Text = std::format("switch ({}) {{ ", Index);
			for (uint16_t CaseIndex = 0; CaseIndex < Count; ++CaseIndex)
			{
				std::string CaseValue;
				std::string CaseResult;
				uint64_t CaseOffset = 0;
				if (!ParseExpression(Depth + 1, CaseValue) || !ReadCodeOffset(CaseOffset) ||
					!ParseExpression(Depth + 1, CaseResult)) return false;
				Text += std::format("case {}: {} /* next=0x{:X} */; ", CaseValue, CaseResult, CaseOffset);
			}
			std::string DefaultValue;
			if (!ParseExpression(Depth + 1, DefaultValue)) return false;
			Text += std::format("default: {} }} /* end=0x{:X} */", DefaultValue, EndOffset);
			return true;
		}

		case EExprToken::EX_TextConst:
		{
			uint8_t TextType = 0;
			if (!Reader.TryReadByte(TextType)) return false;
			switch (TextType)
			{
			case 0: Text = "FText::GetEmpty()"; return true;
			case 1:
			{
				std::string Source;
				std::string Key;
				std::string Namespace;
				if (!ParseExpression(Depth + 1, Source) || !ParseExpression(Depth + 1, Key) ||
					!ParseExpression(Depth + 1, Namespace)) return false;
				Text = std::format("NSLOCTEXT({}, {}, {})", Namespace, Key, Source);
				return true;
			}
			case 2:
			case 3:
			{
				std::string Source;
				if (!ParseExpression(Depth + 1, Source)) return false;
				Text = std::format("{}({})", TextType == 2
					? "FText::AsCultureInvariant" : "FText::FromString", Source);
				return true;
			}
			case 4:
			{
				std::string Table;
				std::string TableId;
				std::string Key;
				if (!ReadPointerToken("Object", Table) || !ParseExpression(Depth + 1, TableId) ||
					!ParseExpression(Depth + 1, Key)) return false;
				Text = std::format("FText::FromStringTable({}, {}, {})", Table, TableId, Key);
				return true;
			}
			case 255: Text = "FText()"; return true;
			default:
				return Reader.Fail(ErrorCode::InvalidOperand, Reader.Position() - 1,
					std::format("unsupported TextConst type {}", TextType));
			}
		}

		case EExprToken::EX_StructMemberContext:
		{
			std::string Property;
			std::string Structure;
			if (!ReadPointerToken("Property", Property) || !ParseExpression(Depth + 1, Structure)) return false;
			Text = std::format("{}.{}", Structure, Property);
			return true;
		}

		case EExprToken::EX_EndOfScript:
			Text = "EndOfScript";
			return true;

		case EExprToken::EX_EndFunctionParms:
		case EExprToken::EX_EndStructConst:
		case EExprToken::EX_EndArray:
		case EExprToken::EX_EndArrayConst:
		case EExprToken::EX_EndSet:
		case EExprToken::EX_EndSetConst:
		case EExprToken::EX_EndMap:
		case EExprToken::EX_EndMapConst:
		case EExprToken::EX_EndParmValue:
			return Reader.Fail(ErrorCode::UnexpectedTerminator, Reader.Position() - 1,
				std::format("unexpected {} terminator", GetExprTokenName(Token)));

		default:
			return Reader.Fail(ErrorCode::UnsupportedOpcode, Reader.Position() - 1,
				std::format("mapped token {} has no bounded decoder", GetExprTokenName(Token)));
		}
	}

	std::span<const uint8_t> Script;
	const Profile& ProfileData;
	BytecodeReader Reader;
	Result Output;
};
}

BlueprintDecompiler::BytecodeProfile& BlueprintDecompiler::BytecodeProfile::MapOpcode(
	const uint8_t RawOpcode,
	const EExprToken Token)
{
	Opcodes[RawOpcode] = { OpcodeSemantic::ExprToken, Token };
	return *this;
}

BlueprintDecompiler::BytecodeProfile& BlueprintDecompiler::BytecodeProfile::MapPrimitiveCast(
	const uint8_t RawOpcode)
{
	Opcodes[RawOpcode] = { OpcodeSemantic::PrimitiveCast, EExprToken::EX_Max };
	return *this;
}

BlueprintDecompiler::DisassemblyResult BlueprintDecompiler::Disassemble(
	const std::span<const uint8_t> Script,
	const BytecodeProfile& Profile)
{
	DisassemblyResult ResultValue;
	ResultValue.ProfileId = Profile.Id;
	ResultValue.InputSize = Script.size();

	std::string ProfileError;
	if (!ValidateProfile(Profile, ProfileError))
	{
		ResultValue.Status = DisassemblyStatus::Error;
		ResultValue.FirstError = {
			true,
			0,
			Profile.Id.empty() ? DisassemblyErrorCode::ProfileRequired : DisassemblyErrorCode::InvalidProfile,
			std::move(ProfileError),
		};
		return ResultValue;
	}
	if (Script.size() > Profile.Limits.MaxInputBytes)
	{
		ResultValue.Status = DisassemblyStatus::Error;
		ResultValue.FirstError = {
			true,
			0,
			DisassemblyErrorCode::InputLimitExceeded,
			"script exceeds profile input-size limit",
		};
		return ResultValue;
	}

	return BoundedParser(Script, Profile).Run();
}

std::string BlueprintDecompiler::DecompileBytes(const std::vector<uint8_t>& Script)
{
	(void)Script;
	return std::string(ProfileRequiredText);
}

BlueprintDecompiler::DecompileResult BlueprintDecompiler::Decompile(const UEFunction& Func)
{
	(void)Func;
	DecompileResult ResultValue;
	ResultValue.Pseudocode = std::string(ProfileRequiredText);
	return ResultValue;
}
