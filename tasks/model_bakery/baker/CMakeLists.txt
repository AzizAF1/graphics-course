add_executable(model_bakery_baker
  main.cpp
)

target_link_libraries(model_bakery_baker
  PRIVATE tinygltf
)

target_compile_features(model_bakery_baker PRIVATE cxx_std_17)
