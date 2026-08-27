file(REMOVE_RECURSE
  "libtweeny-dummy.a"
  "libtweeny-dummy.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/tweeny-dummy.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
