#!/usr/bin/env bash

########################### Teamcity Build Step: Command Line #######################
: <<EOF
#!/bin/bash
export PATH=/usr/local/software/jdk1.8.0_131/bin:/usr/local/software/apache-maven-3.6.3/bin:${PATH}
if [[ -f "${teamcity_build_checkoutDir:-}"/regression-test/pipeline/cloud_p0/run.sh ]]; then
    cd "${teamcity_build_checkoutDir}"/regression-test/pipeline/cloud_p0/
    bash -x run.sh
else
    echo "Build Step file missing: regression-test/pipeline/cloud_p0/run.sh" && exit 1
fi
EOF
############################# run.sh content ########################################
# shellcheck source=/dev/null
# check_tpcds_table_rows, restart_doris, set_session_variable, check_tpcds_result
source "${teamcity_build_checkoutDir}"/regression-test/pipeline/common/doris-utils.sh
# shellcheck source=/dev/null
# create_an_issue_comment
source "${teamcity_build_checkoutDir}"/regression-test/pipeline/common/github-utils.sh
# shellcheck source=/dev/null
# upload_doris_log_to_oss
source "${teamcity_build_checkoutDir}"/regression-test/pipeline/common/oss-utils.sh

if ${DEBUG:-false}; then
    pr_num_from_trigger=${pr_num_from_debug:-"30772"}
    commit_id_from_trigger=${commit_id_from_debug:-"8a0077c2cfc492894d9ff68916e7e131f9a99b65"}
fi
echo "#### Check env"
if [[ -z "${teamcity_build_checkoutDir}" ]]; then echo "ERROR: env teamcity_build_checkoutDir not set" && exit 1; fi
if [[ -z "${pr_num_from_trigger}" ]]; then echo "ERROR: env pr_num_from_trigger not set" && exit 1; fi
if [[ -z "${commit_id_from_trigger}" ]]; then echo "ERROR: env commit_id_from_trigger not set" && exit 1; fi
if [[ -z "${cos_ak}" || -z "${cos_sk}" ]]; then echo "ERROR: env cos_ak or cos_sk not set" && exit 1; fi

# shellcheck source=/dev/null
source "$(bash "${teamcity_build_checkoutDir}"/regression-test/pipeline/common/get-or-set-tmp-env.sh 'get')"
if ${skip_pipeline:=false}; then echo "INFO: skip build pipline" && exit 0; else echo "INFO: no skip"; fi

echo "#### Run tpcds test on Doris ####"
DORIS_HOME="${teamcity_build_checkoutDir}/output"
export DORIS_HOME
exit_flag=0

# shellcheck disable=SC2317
run() {
    set -e
    shopt -s inherit_errexit

    cd "${teamcity_build_checkoutDir}" || return 1
    echo "ak='${cos_ak}'" >>"${teamcity_build_checkoutDir}"/regression-test/pipeline/cloud_p0/conf/regression-conf-custom.groovy
    echo "sk='${cos_sk}'" >>"${teamcity_build_checkoutDir}"/regression-test/pipeline/cloud_p0/conf/regression-conf-custom.groovy
    cp -f "${teamcity_build_checkoutDir}"/regression-test/pipeline/cloud_p0/conf/regression-conf-custom.groovy \
        "${teamcity_build_checkoutDir}"/regression-test/conf/
    if "${teamcity_build_checkoutDir}"/run-regression-test.sh \
        --teamcity \
        --clean \
        --run \
        --times "${repeat_times_from_trigger:-1}" \
        -parallel 14 \
        -suiteParallel 14 \
        -actionParallel 2; then
        echo
    else
        # regression 测试跑完后输出的汇总信息，Test 1961 suites, failed 1 suites, fatal 0 scripts, skipped 0 scripts
        # 如果 test_suites>0 && failed_suites<=3  && fatal_scripts=0，就把返回状态码改为正常的0，让teamcity根据跑case的情况去判断成功还是失败
        # 这样预期能够快速 mute 不稳定的 case
        summary=$(
            grep -aoE 'Test ([0-9]+) suites, failed ([0-9]+) suites, fatal ([0-9]+) scripts, skipped ([0-9]+) scripts' \
                "${DORIS_HOME}"/regression-test/log/doris-regression-test.*.log
        )
        set -x
        test_suites=$(echo "${summary}" | cut -d ' ' -f 2)
        failed_suites=$(echo "${summary}" | cut -d ' ' -f 5)
        fatal_scripts=$(echo "${summary}" | cut -d ' ' -f 8)
        if [[ ${test_suites} -gt 0 && ${failed_suites} -le 30 && ${fatal_scripts} -eq 0 ]]; then
            echo "INFO: regression test result meet (test_suites>0 && failed_suites<=30 && fatal_scripts=0)"
        else
            return 1
        fi
    fi
}
export -f run
# 设置超时时间（以分为单位）
timeout_minutes=$((${repeat_times_from_trigger:-1} * 90))m
timeout "${timeout_minutes}" bash -cx run
exit_flag="$?"

echo "#### 5. check if need backup doris logs"
if [[ ${exit_flag} != "0" ]]; then
    check_if_need_gcore
    stop_doris
    print_doris_fe_log
    print_doris_be_log
    if file_name=$(archive_doris_coredump "${pr_num_from_trigger}_${commit_id_from_trigger}_doris_coredump.tar.gz"); then
        upload_doris_log_to_oss "${file_name}"
    fi
    if file_name=$(archive_doris_logs "${pr_num_from_trigger}_${commit_id_from_trigger}_doris_logs.tar.gz"); then
        upload_doris_log_to_oss "${file_name}"
    fi
fi

exit "${exit_flag}"
