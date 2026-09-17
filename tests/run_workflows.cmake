# Every GitHub Actions workflow runs only when started by hand. Hosted build
# minutes are limited, so no workflow may run on a push, a pull request or a
# schedule. Run with cmake -P and WORKFLOWS, the .github/workflows directory.

file(GLOB workflows "${WORKFLOWS}/*.yml" "${WORKFLOWS}/*.yaml")
foreach(workflow IN LISTS workflows)
    file(READ "${workflow}" content)
    if(content MATCHES "\n  (push|pull_request|pull_request_target|schedule|workflow_run|release):")
        message(FATAL_ERROR "${workflow} runs on ${CMAKE_MATCH_1}")
    endif()
    if(NOT content MATCHES "\non:\n  workflow_dispatch:\n")
        message(FATAL_ERROR "${workflow} does not run on workflow_dispatch alone")
    endif()
endforeach()
