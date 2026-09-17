# Write the assembly file of a program with antic -S and assemble it with
# llvm-mc, to show that the emitted file is valid for the target. Run with
# cmake -P and these values:
#   ANTIC     the antic executable
#   LLVM_MC   the llvm-mc executable
#   TARGET    the antic target name
#   TRIPLE    the llvm-mc triple of the target
#   SOURCE    the .anti file
#   OPTIONS   optional options of antic, separated by |
#   WORK      a directory for the assembly and object files

if(NOT EXISTS "${LLVM_MC}")
    message(FATAL_ERROR
        "llvm-mc not found at '${LLVM_MC}'. Configure with -DANTIC_LLVM_MC=<path>.")
endif()

get_filename_component(name "${SOURCE}" NAME_WE)
set(assembly "${WORK}/${name}.${TARGET}.s")
file(MAKE_DIRECTORY "${WORK}")
string(REPLACE "|" ";" options "${OPTIONS}")
execute_process(
    COMMAND "${ANTIC}" -S --target "${TARGET}" ${options} -o "${assembly}"
            "${SOURCE}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT out STREQUAL "" OR NOT err STREQUAL "")
    message(FATAL_ERROR "antic -S failed with ${status}\n${out}${err}")
endif()

execute_process(
    COMMAND "${LLVM_MC}" "-triple=${TRIPLE}" -filetype=obj
            -o "${WORK}/${name}.${TARGET}.o" "${assembly}"
    RESULT_VARIABLE status
    ERROR_VARIABLE err)
if(NOT status EQUAL 0 OR NOT err STREQUAL "")
    message(FATAL_ERROR "llvm-mc failed for ${assembly}\n${err}")
endif()
