file(REMOVE_RECURSE
  "bt_cs_soc_reflector.exe"
  "bt_cs_soc_reflector.exe.manifest"
  "bt_cs_soc_reflector.pdb"
  "libbt_cs_soc_reflector.dll.a"
)

# Per-language clean rules from dependency scanning.
foreach(lang ASM C)
  include(CMakeFiles/bt_cs_soc_reflector.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
