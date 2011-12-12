
TOP = ../../../..
include $(TOP)/configs/current

include Makefile.sources

LIBNAME = radeon

LIBRARY_INCLUDES = -I$(TOP)/include

TBLGEN = $(LLVM_BINDIR)/tblgen

CXXFLAGS+= $(LLVM_CXXFLAGS)

include ../../Makefile.template

tablegen = $(TBLGEN) -I $(LLVM_INCLUDEDIR) $1 $2 -o $3

gen: $(GENERATED_SOURCES)

R600ShaderPatterns.td: AMDISAGenShaderPatterns.pl
	$(PERL) $^ C > $@
	
R600RegisterInfo.td: R600GenRegisterInfo.pl
	$(PERL) $^ > $@

AMDISAInstrEnums.td: AMDISAGenInstrEnums.pl
	$(PERL) $^ td > $@

AMDISAInstrEnums.h.inc: AMDISAGenInstrEnums.pl
	$(PERL) $^ h > $@

AMDISAInstrEnums.inc: AMDISAGenInstrEnums.pl
	$(PERL) $^ inc > $@

AMDILGenRegisterInfo.h.inc: *.td
	$(call tablegen, -gen-register-desc-header, AMDIL.td, $@)

AMDILGenRegisterNames.inc: *.td
	$(call tablegen, -gen-register-enums, AMDIL.td, $@)

AMDILGenRegisterInfo.inc: *.td
	$(call tablegen, -gen-register-desc, AMDIL.td, $@)

AMDILGenInstrNames.inc: *.td
	$(call tablegen, -gen-instr-enums, AMDIL.td, $@)

AMDILGenInstrInfo.inc: *.td
	$(call tablegen, -gen-instr-desc, AMDIL.td, $@)

AMDILGenAsmWriter.inc: *.td
	$(call tablegen, -gen-asm-writer, AMDIL.td, $@)

AMDILGenDAGISel.inc: *.td
	$(call tablegen, -gen-dag-isel, AMDIL.td, $@)

AMDILGenCallingConv.inc: *.td
	$(call tablegen, -gen-callingconv, AMDIL.td, $@)

AMDILGenSubtarget.inc: *.td
	$(call tablegen, -gen-subtarget, AMDIL.td, $@)

AMDILGenEDInfo.inc: *.td
	$(call tablegen, -gen-enhanced-disassembly-info, AMDIL.td, $@)

AMDILGenIntrinsics.inc: *.td
	$(call tablegen, -gen-tgt-intrinsic, AMDIL.td, $@)

AMDISAGenCodeEmitter.inc: *.td
	$(call tablegen, -gen-emitter, AMDIL.td, $@)

%.td: ;


