if [ "$(uname -s)" = Linux ]; then
  read -ra CXX_ARGS <<< "$CXX"
  run "${CXX_ARGS[@]}" -std=c++20 -DLEAN_MULTI_THREAD -I "$SRC_DIR" \
    "$TEST_DIR/misc/available_parallelism.cpp" -o "$TMP_DIR/available_parallelism"
  run "$TMP_DIR/available_parallelism"
fi
