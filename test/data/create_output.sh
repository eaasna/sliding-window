#!/bin/bash

export VALIK_TMP=tmp/valik/my_dir
export VALIK_STELLAR=echo

./split/cli_test_output.sh

./build/cli_test_output.sh

./search/cli_test_output.sh

./consolidate/cli_test_output.sh