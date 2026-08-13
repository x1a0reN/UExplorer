#pragma once

#include <cstdint>

// Parser semantic identities. Raw opcode values are deliberately supplied by
// BytecodeProfile because UE4 and UE5 assign several EExprToken values
// differently.
enum class EExprToken : uint8_t
{
	EX_LocalVariable,
	EX_InstanceVariable,
	EX_DefaultVariable,
	EX_Return,
	EX_Jump,
	EX_JumpIfNot,
	EX_Assert,
	EX_Nothing,
	EX_NothingInt32,
	EX_Let,
	EX_BitFieldConst,
	EX_ClassContext,
	EX_MetaCast,
	EX_LetBool,
	EX_EndParmValue,
	EX_EndFunctionParms,
	EX_Self,
	EX_Skip,
	EX_Context,
	EX_Context_FailSilent,
	EX_VirtualFunction,
	EX_FinalFunction,
	EX_IntConst,
	EX_FloatConst,
	EX_StringConst,
	EX_ObjectConst,
	EX_NameConst,
	EX_RotationConst,
	EX_VectorConst,
	EX_ByteConst,
	EX_IntZero,
	EX_IntOne,
	EX_True,
	EX_False,
	EX_TextConst,
	EX_NoObject,
	EX_TransformConst,
	EX_IntConstByte,
	EX_NoInterface,
	EX_DynamicCast,
	EX_StructConst,
	EX_EndStructConst,
	EX_SetArray,
	EX_EndArray,
	EX_PropertyConst,
	EX_UnicodeStringConst,
	EX_Int64Const,
	EX_UInt64Const,
	EX_DoubleConst,
	EX_Vector3fConst,
	EX_SetSet,
	EX_EndSet,
	EX_SetMap,
	EX_EndMap,
	EX_SetConst,
	EX_EndSetConst,
	EX_MapConst,
	EX_EndMapConst,
	EX_StructMemberContext,
	EX_LetMulticastDelegate,
	EX_LetDelegate,
	EX_LocalVirtualFunction,
	EX_LocalFinalFunction,
	EX_LocalOutVariable,
	EX_DeprecatedOp4A,
	EX_InstanceDelegate,
	EX_PushExecutionFlow,
	EX_PopExecutionFlow,
	EX_ComputedJump,
	EX_PopExecutionFlowIfNot,
	EX_Breakpoint,
	EX_InterfaceContext,
	EX_ObjToInterfaceCast,
	EX_EndOfScript,
	EX_CrossInterfaceCast,
	EX_InterfaceToObjCast,
	EX_WireTracepoint,
	EX_SkipOffsetConst,
	EX_AddMulticastDelegate,
	EX_ClearMulticastDelegate,
	EX_Tracepoint,
	EX_LetObj,
	EX_LetWeakObjPtr,
	EX_BindDelegate,
	EX_RemoveMulticastDelegate,
	EX_CallMulticastDelegate,
	EX_LetValueOnPersistentFrame,
	EX_ArrayConst,
	EX_EndArrayConst,
	EX_SoftObjectConst,
	EX_CallMath,
	EX_SwitchValue,
	EX_InstrumentationEvent,
	EX_ArrayGetByRef,
	EX_ClassSparseDataVariable,
	EX_FieldPathConst,
	EX_AutoRtfmTransact,
	EX_AutoRtfmStopTransact,
	EX_AutoRtfmAbortIfNot,
	EX_Max,
};

// Get opcode name string
inline const char* GetExprTokenName(EExprToken Token)
{
	switch (Token)
	{
	case EExprToken::EX_LocalVariable:        return "LocalVariable";
	case EExprToken::EX_InstanceVariable:     return "InstanceVariable";
	case EExprToken::EX_DefaultVariable:      return "DefaultVariable";
	case EExprToken::EX_Return:               return "Return";
	case EExprToken::EX_Jump:                 return "Jump";
	case EExprToken::EX_JumpIfNot:            return "JumpIfNot";
	case EExprToken::EX_Assert:               return "Assert";
	case EExprToken::EX_Nothing:              return "Nothing";
	case EExprToken::EX_NothingInt32:         return "NothingInt32";
	case EExprToken::EX_Let:                  return "Let";
	case EExprToken::EX_BitFieldConst:        return "BitFieldConst";
	case EExprToken::EX_ClassContext:         return "ClassContext";
	case EExprToken::EX_MetaCast:             return "MetaCast";
	case EExprToken::EX_LetBool:              return "LetBool";
	case EExprToken::EX_EndParmValue:         return "EndParmValue";
	case EExprToken::EX_EndFunctionParms:     return "EndFunctionParms";
	case EExprToken::EX_Self:                 return "Self";
	case EExprToken::EX_Skip:                 return "Skip";
	case EExprToken::EX_Context:              return "Context";
	case EExprToken::EX_Context_FailSilent:   return "Context_FailSilent";
	case EExprToken::EX_VirtualFunction:      return "VirtualFunction";
	case EExprToken::EX_FinalFunction:        return "FinalFunction";
	case EExprToken::EX_IntConst:             return "IntConst";
	case EExprToken::EX_FloatConst:           return "FloatConst";
	case EExprToken::EX_StringConst:          return "StringConst";
	case EExprToken::EX_ObjectConst:          return "ObjectConst";
	case EExprToken::EX_NameConst:            return "NameConst";
	case EExprToken::EX_RotationConst:        return "RotationConst";
	case EExprToken::EX_VectorConst:          return "VectorConst";
	case EExprToken::EX_ByteConst:            return "ByteConst";
	case EExprToken::EX_IntZero:              return "IntZero";
	case EExprToken::EX_IntOne:               return "IntOne";
	case EExprToken::EX_True:                 return "True";
	case EExprToken::EX_False:                return "False";
	case EExprToken::EX_TextConst:            return "TextConst";
	case EExprToken::EX_NoObject:             return "NoObject";
	case EExprToken::EX_TransformConst:        return "TransformConst";
	case EExprToken::EX_IntConstByte:         return "IntConstByte";
	case EExprToken::EX_NoInterface:          return "NoInterface";
	case EExprToken::EX_DynamicCast:          return "DynamicCast";
	case EExprToken::EX_StructConst:          return "StructConst";
	case EExprToken::EX_EndStructConst:       return "EndStructConst";
	case EExprToken::EX_SetArray:             return "SetArray";
	case EExprToken::EX_EndArray:             return "EndArray";
	case EExprToken::EX_PropertyConst:        return "PropertyConst";
	case EExprToken::EX_UnicodeStringConst:   return "UnicodeStringConst";
	case EExprToken::EX_Int64Const:           return "Int64Const";
	case EExprToken::EX_UInt64Const:          return "UInt64Const";
	case EExprToken::EX_DoubleConst:          return "DoubleConst";
	case EExprToken::EX_Vector3fConst:        return "Vector3fConst";
	case EExprToken::EX_SetSet:               return "SetSet";
	case EExprToken::EX_EndSet:               return "EndSet";
	case EExprToken::EX_SetMap:               return "SetMap";
	case EExprToken::EX_EndMap:               return "EndMap";
	case EExprToken::EX_SetConst:             return "SetConst";
	case EExprToken::EX_EndSetConst:          return "EndSetConst";
	case EExprToken::EX_MapConst:             return "MapConst";
	case EExprToken::EX_EndMapConst:          return "EndMapConst";
	case EExprToken::EX_StructMemberContext:  return "StructMemberContext";
	case EExprToken::EX_LetMulticastDelegate: return "LetMulticastDelegate";
	case EExprToken::EX_LetDelegate:          return "LetDelegate";
	case EExprToken::EX_LocalVirtualFunction: return "LocalVirtualFunction";
	case EExprToken::EX_LocalFinalFunction:   return "LocalFinalFunction";
	case EExprToken::EX_LocalOutVariable:     return "LocalOutVariable";
	case EExprToken::EX_DeprecatedOp4A:       return "DeprecatedOp4A";
	case EExprToken::EX_InstanceDelegate:     return "InstanceDelegate";
	case EExprToken::EX_PushExecutionFlow:    return "PushExecutionFlow";
	case EExprToken::EX_PopExecutionFlow:     return "PopExecutionFlow";
	case EExprToken::EX_ComputedJump:         return "ComputedJump";
	case EExprToken::EX_PopExecutionFlowIfNot: return "PopExecutionFlowIfNot";
	case EExprToken::EX_Breakpoint:           return "Breakpoint";
	case EExprToken::EX_InterfaceContext:     return "InterfaceContext";
	case EExprToken::EX_ObjToInterfaceCast:   return "ObjToInterfaceCast";
	case EExprToken::EX_EndOfScript:          return "EndOfScript";
	case EExprToken::EX_CrossInterfaceCast:   return "CrossInterfaceCast";
	case EExprToken::EX_InterfaceToObjCast:   return "InterfaceToObjCast";
	case EExprToken::EX_WireTracepoint:       return "WireTracepoint";
	case EExprToken::EX_SkipOffsetConst:      return "SkipOffsetConst";
	case EExprToken::EX_AddMulticastDelegate: return "AddMulticastDelegate";
	case EExprToken::EX_ClearMulticastDelegate: return "ClearMulticastDelegate";
	case EExprToken::EX_Tracepoint:           return "Tracepoint";
	case EExprToken::EX_LetObj:               return "LetObj";
	case EExprToken::EX_LetWeakObjPtr:        return "LetWeakObjPtr";
	case EExprToken::EX_BindDelegate:         return "BindDelegate";
	case EExprToken::EX_RemoveMulticastDelegate: return "RemoveMulticastDelegate";
	case EExprToken::EX_CallMulticastDelegate: return "CallMulticastDelegate";
	case EExprToken::EX_LetValueOnPersistentFrame: return "LetValueOnPersistentFrame";
	case EExprToken::EX_ArrayConst:           return "ArrayConst";
	case EExprToken::EX_EndArrayConst:        return "EndArrayConst";
	case EExprToken::EX_SoftObjectConst:      return "SoftObjectConst";
	case EExprToken::EX_CallMath:             return "CallMath";
	case EExprToken::EX_SwitchValue:          return "SwitchValue";
	case EExprToken::EX_InstrumentationEvent: return "InstrumentationEvent";
	case EExprToken::EX_ArrayGetByRef:        return "ArrayGetByRef";
	case EExprToken::EX_ClassSparseDataVariable: return "ClassSparseDataVariable";
	case EExprToken::EX_FieldPathConst:       return "FieldPathConst";
	case EExprToken::EX_AutoRtfmTransact:     return "AutoRtfmTransact";
	case EExprToken::EX_AutoRtfmStopTransact: return "AutoRtfmStopTransact";
	case EExprToken::EX_AutoRtfmAbortIfNot:   return "AutoRtfmAbortIfNot";
	case EExprToken::EX_Max:                  return "Invalid";
	default:                                  return "Unknown";
	}
}
