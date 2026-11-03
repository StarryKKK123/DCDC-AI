################################################################################
# Automatically-generated file. Do not edit!
################################################################################

SHELL = cmd.exe

# Each subdirectory must supply rules for building sources it contributes
%.obj: ../%.c $(GEN_OPTS) | $(GEN_FILES) $(GEN_MISC_FILES)
	@echo 'C2000 Compiler - building file: "$<"'
	"E:/MyTool/TI/CCS/ccs/tools/compiler/ti-cgt-c2000_25.11.1.LTS/bin/cl2000" -v28 -ml -mt --cla_support=cla2 --float_support=fpu32 --tmu_support=tmu0 --vcu_support=vcrc -Ooff --include_path="E:/TI/project/openloop_300W" --include_path="E:/TI/c2000ware/driverlib/f28p55x/driverlib" --include_path="E:/TI/project/openloop_300W/device" --include_path="E:/MyTool/TI/CCS/ccs/tools/compiler/ti-cgt-c2000_25.11.1.LTS/include" --define=DEBUG --define=_FLASH --define=_LAUNCHXL_F28P55X --c99 --diag_suppress=10063 --diag_warning=225 --diag_wrap=off --display_error_number --gen_func_subsections=on --abi=eabi --preproc_with_compile --preproc_dependency="$(basename $(<F)).d_raw" $(GEN_OPTS__FLAG) "$<"
	@echo 'Finished building: "$<"'
	@echo ' '


