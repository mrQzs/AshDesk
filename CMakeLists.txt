cmake_minimum_required(VERSION 3.24)
# AshDesk 桌面程序：Qt 界面、液态玻璃内核、自动更新器、安装与卸载脚本。
# 远程桌面的引擎（主机、客户端、系统服务）不在这个仓库里：界面只经命令行和控制管道和它打交道（docs/核心接口.md），
# 正式安装包由引擎仓库的发布脚本把引擎、本仓库和驱动拼在一起。不带引擎也能编、能跑界面测试（假核心）。
project(AshDesk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 版本号只在上面的 project(VERSION) 里改，和引擎的版本号保持一致（发布脚本核对）
configure_file(ui/ashdesk_version.h.in ${PROJECT_BINARY_DIR}/version/ashdesk_version.h @ONLY)
# 构建号：每次构建都跑，内容变了才重写 version/ashdesk_build.h
add_custom_target(ashdesk_build_id
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${PROJECT_BINARY_DIR}/version/ashdesk_build.h
          -P ${CMAKE_SOURCE_DIR}/tools/ashdesk_build_id.cmake
  BYPRODUCTS ${PROJECT_BINARY_DIR}/version/ashdesk_build.h)
# Windows exe 的图标与版本资源（ui/ashdesk.rc），文件名和描述按 exe 分别给
function(ashdesk_add_rc target description)
  target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/ui/ashdesk.rc)
  target_include_directories(${target} PRIVATE ${PROJECT_BINARY_DIR}/version)
  add_dependencies(${target} ashdesk_build_id)
  target_compile_definitions(${target} PRIVATE
    "$<$<COMPILE_LANGUAGE:RC>:ASHDESK_RC_NAME=\"${target}\";ASHDESK_RC_FILE=\"${target}.exe\";ASHDESK_RC_DESCRIPTION=\"${description}\">")
endfunction()

if(MSVC)
  set(ASHDESK_WARN /utf-8 /W4 /WX /fp:precise /GS /D_CRT_SECURE_NO_WARNINGS)
else()
  set(ASHDESK_WARN -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

# macOS：AshDesk.app 的图标、权限说明的各语言文字；Info.plist 模板是 packaging/macos/Info.plist.in
if(APPLE)
  set(ASHDESK_MAC_RESOURCES ${CMAKE_SOURCE_DIR}/ui/icons/app/ashdesk.icns ${CMAKE_SOURCE_DIR}/packaging/macos/en.lproj/InfoPlist.strings
                            ${CMAKE_SOURCE_DIR}/packaging/macos/zh-Hans.lproj/InfoPlist.strings)
  set_source_files_properties(${CMAKE_SOURCE_DIR}/ui/icons/app/ashdesk.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set_source_files_properties(${CMAKE_SOURCE_DIR}/packaging/macos/en.lproj/InfoPlist.strings PROPERTIES MACOSX_PACKAGE_LOCATION Resources/en.lproj)
  set_source_files_properties(${CMAKE_SOURCE_DIR}/packaging/macos/zh-Hans.lproj/InfoPlist.strings PROPERTIES MACOSX_PACKAGE_LOCATION Resources/zh-Hans.lproj)
endif()

# ---------------- 液态玻璃像素内核（界面渲染用）：C17 + SSE2 / NEON 汇编，不依赖 Qt ----------------
add_library(ashdesk_glass STATIC glass/ad_glass.c)
target_include_directories(ashdesk_glass PUBLIC glass)
target_compile_options(ashdesk_glass PRIVATE "$<$<COMPILE_LANGUAGE:C>:${ASHDESK_WARN}>")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
  enable_language(ASM_NASM)
  if(WIN32)
    set(CMAKE_ASM_NASM_FLAGS "${CMAKE_ASM_NASM_FLAGS} -DAD_WIN64")
  elseif(APPLE)
    set(CMAKE_ASM_NASM_FLAGS "${CMAKE_ASM_NASM_FLAGS} -DAD_MACHO")
  endif()
  target_sources(ashdesk_glass PRIVATE glass/ad_glass_x64.asm)
  target_compile_definitions(ashdesk_glass PRIVATE AD_GLASS_ASM_X64)   # 汇编真的进了库才定义，C 侧按它选行内核
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64" AND NOT MSVC)
  enable_language(ASM)
  if(APPLE)
    target_compile_definitions(ashdesk_glass PRIVATE AD_MACHO)
  endif()
  target_sources(ashdesk_glass PRIVATE glass/ad_glass_a64.S)
  target_compile_definitions(ashdesk_glass PRIVATE AD_GLASS_ASM_A64)
endif()

enable_testing()
add_executable(test_glass tests/test_glass.c)
target_link_libraries(test_glass PRIVATE ashdesk_glass)
if(NOT WIN32)
  target_link_libraries(test_glass PRIVATE m)
endif()
target_compile_options(test_glass PRIVATE ${ASHDESK_WARN})
add_test(NAME test_glass COMMAND test_glass)

# GoogleTest 1.18（BSD-3，third_party/googletest/README.AshDesk.md）：Windows 用 vcpkg 编出来放进 third_party/googletest，
# 别的平台用系统的包
if(WIN32)
  set(ASHDESK_GTEST_DIR ${CMAKE_SOURCE_DIR}/third_party/googletest)
  if(EXISTS ${ASHDESK_GTEST_DIR}/lib/gtest.lib AND EXISTS ${ASHDESK_GTEST_DIR}/lib/gtest_main.lib)
    add_library(GTest::gtest STATIC IMPORTED)
    set_target_properties(GTest::gtest PROPERTIES IMPORTED_LOCATION ${ASHDESK_GTEST_DIR}/lib/gtest.lib
                                                  INTERFACE_INCLUDE_DIRECTORIES ${ASHDESK_GTEST_DIR}/include)
    set(ASHDESK_HAVE_GTEST ON)
  endif()
else()
  find_package(GTest QUIET)
  set(ASHDESK_HAVE_GTEST ${GTest_FOUND})
endif()

# ---------------- 自动更新器（Windows）：单独的进程，验 Ed25519 签名后整目录换新，失败换回 ----------------
if(WIN32)
  # OpenSSL 固定为 3.5 LTS 的 3.5.9（third_party/openssl/README.AshDesk.md）：用 vcpkg 编出来放进 third_party/openssl，
  # 配置时核对哈希与版本号。-DOPENSSL_ROOT_DIR=... 可以换别的 OpenSSL，但那样不校验
  set(ASHDESK_OPENSSL_VERSION 3.5.9)
  set(ASHDESK_OPENSSL_DIR ${CMAKE_SOURCE_DIR}/third_party/openssl)
  if(NOT OPENSSL_ROOT_DIR)
    if(NOT EXISTS ${ASHDESK_OPENSSL_DIR}/include/openssl/evp.h)
      message(FATAL_ERROR "缺 third_party/openssl（OpenSSL ${ASHDESK_OPENSSL_VERSION}），取得方式见 third_party/openssl/README.AshDesk.md")
    endif()
    file(STRINGS ${ASHDESK_OPENSSL_DIR}/SHA256SUMS ossl_sums)
    foreach(line ${ossl_sums})
      string(REGEX MATCH "^([0-9a-f]+)  (.+)$" _ "${line}")
      file(SHA256 ${ASHDESK_OPENSSL_DIR}/${CMAKE_MATCH_2} got)
      if(NOT got STREQUAL CMAKE_MATCH_1)
        message(FATAL_ERROR "third_party/openssl/${CMAKE_MATCH_2} 的哈希和 SHA256SUMS 不符")
      endif()
    endforeach()
    set(OPENSSL_ROOT_DIR ${ASHDESK_OPENSSL_DIR})
  endif()
  find_package(OpenSSL 3.2 REQUIRED COMPONENTS Crypto)
  get_filename_component(OPENSSL_BIN_DIR "${OPENSSL_INCLUDE_DIR}/../bin" ABSOLUTE)

  add_executable(ashdesk_update updater/update_main.c updater/upd_core.c)
  ashdesk_add_rc(ashdesk_update "AshDesk Update")
  target_link_libraries(ashdesk_update PRIVATE OpenSSL::Crypto winhttp bcrypt advapi32 version shell32)
  target_compile_definitions(ashdesk_update PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  target_compile_options(ashdesk_update PRIVATE ${ASHDESK_WARN})
  add_custom_command(TARGET ashdesk_update POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENSSL_BIN_DIR}/libcrypto-3-x64.dll" $<TARGET_FILE_DIR:ashdesk_update>)

  add_executable(test_update tests/test_update.c updater/upd_core.c)
  target_include_directories(test_update PRIVATE updater)
  target_link_libraries(test_update PRIVATE OpenSSL::Crypto bcrypt)
  target_compile_definitions(test_update PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX "UPD_PUBKEY_PEM=\"${CMAKE_SOURCE_DIR}/packaging/update_pubkey.pem\"")
  target_compile_options(test_update PRIVATE ${ASHDESK_WARN})
  add_custom_command(TARGET test_update POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENSSL_BIN_DIR}/libcrypto-3-x64.dll" $<TARGET_FILE_DIR:test_update>)
  add_test(NAME test_update COMMAND test_update)
endif()

# ---------------- Qt 界面 ----------------
# 引擎的安装目录（引擎仓库 cmake --install 的前缀）。只有 macOS 打包要它：引擎的 adrd_client.app 套进 AshDesk.app/Contents/Helpers
set(ASHDESK_ENGINE_DIR "" CACHE PATH "引擎的安装目录（macOS 打包时取其中的 adrd_client.app）")
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network Svg LinguistTools)
add_library(ashdesk_widgets STATIC ui/desktop_options.cpp ui/core_process.cpp ui/liquid_glass.cpp
  ui/controls.cpp ui/main_window.cpp ui/diagnostics.cpp ui/firewall.cpp ui/driver_install.cpp)
set_target_properties(ashdesk_widgets PROPERTIES AUTOMOC ON)
target_compile_features(ashdesk_widgets PUBLIC cxx_std_17)
target_include_directories(ashdesk_widgets PUBLIC ui PRIVATE ${PROJECT_BINARY_DIR}/version)
target_link_libraries(ashdesk_widgets PUBLIC Qt6::Widgets Qt6::Network ashdesk_glass PRIVATE Qt6::Svg)
if(WIN32)
  # ui/firewall.cpp 用系统防火墙的 COM 接口（INetFwPolicy2）查规则；ui/driver_install.cpp 用 ShellExecuteEx 提权跑安装脚本
  target_link_libraries(ashdesk_widgets PRIVATE ole32 oleaut32 shell32)
endif()
# ui/icons/app/* 由 tools/make_app_icon.py 从 Logo 生成，不要手改
set(ASHDESK_APP_ICON_PNGS)
foreach(size 16 20 24 32 40 48 64 128 256)
  list(APPEND ASHDESK_APP_ICON_PNGS ui/icons/app/ashdesk-${size}.png)
endforeach()
qt_add_resources(ashdesk_widgets "ashdesk_icons" PREFIX "/ashdesk" BASE ui/icons
  FILES ui/icons/chevron-up.svg ui/icons/chevron-down.svg ${ASHDESK_APP_ICON_PNGS})
# 界面源文字是中文。translations/*.ts 用 lupdate 更新（cmake --build <dir> --target update_translations），
# 再补译文；.qm 由构建生成并嵌进资源 :/i18n，不入库
qt_add_translations(TARGETS ashdesk_widgets TS_FILES translations/ashdesk_en.ts RESOURCE_PREFIX /i18n
  LUPDATE_OPTIONS -source-language zh_CN)
target_compile_options(ashdesk_widgets PRIVATE ${ASHDESK_WARN})
add_executable(ashdesk WIN32 MACOSX_BUNDLE ui/main.cpp)
target_include_directories(ashdesk PRIVATE ${PROJECT_BINARY_DIR}/version)   # main.cpp 要版本号
if(WIN32)
  ashdesk_add_rc(ashdesk "AshDesk")
  install(TARGETS ashdesk ashdesk_update RUNTIME DESTINATION bin)
  install(FILES "${OPENSSL_BIN_DIR}/libcrypto-3-x64.dll" DESTINATION bin)
  install(FILES third_party/openssl/LICENSE.txt DESTINATION licenses/openssl)
  # windeployqt 把 Qt 的 DLL 和插件装进包；VC 运行库由 InstallRequiredSystemLibraries 装。
  # Widgets 界面纯 CPU 绘制，用不到 OpenGL 软渲染和 D3D / DXC 着色器编译器
  qt_generate_deploy_app_script(TARGET ashdesk OUTPUT_SCRIPT ashdesk_deploy NO_COMPILER_RUNTIME
    DEPLOY_TOOL_OPTIONS --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler)
  install(SCRIPT ${ashdesk_deploy})
  include(InstallRequiredSystemLibraries)
  # 包根目录的安装、卸载脚本：防火墙规则、开始菜单项、系统服务、驱动、OpenH264
  install(FILES packaging/windows/install.ps1 packaging/windows/uninstall.ps1 packaging/windows/openh264.ps1
                packaging/windows/get-openh264.ps1 packaging/windows/get-openh264.cmd DESTINATION .)
  install(FILES packaging/windows/openh264_BINARY_LICENSE.txt DESTINATION licenses/openh264 RENAME BINARY_LICENSE.txt)
  install(FILES LICENSE NOTICE DESTINATION licenses/ashdesk)
elseif(APPLE)
  target_sources(ashdesk PRIVATE ${ASHDESK_MAC_RESOURCES})
  set_target_properties(ashdesk PROPERTIES OUTPUT_NAME AshDesk MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME AshDesk MACOSX_BUNDLE_GUI_IDENTIFIER com.ashdesk.AshDesk MACOSX_BUNDLE_ICON_FILE ashdesk.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION} MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
  # cmake --install <构建目录> --prefix <目录> 装出能拷走的 AshDesk.app：给了 ASHDESK_ENGINE_DIR 就把引擎的客户端套进
  # Contents/Helpers，用外层的 Qt（它自己的 qt.conf 指过去）；macdeployqt 把两个程序要的 Qt 收进外层；最后整包本机临时签名。
  # 分发要 Developer ID 逐个签 + 公证（引擎仓库的发布脚本做）
  find_program(ASHDESK_MACDEPLOYQT macdeployqt HINTS ${Qt6_DIR}/../../../bin REQUIRED)
  install(TARGETS ashdesk BUNDLE DESTINATION .)
  set(ASHDESK_DEPLOY_EXTRA "")
  if(ASHDESK_ENGINE_DIR)
    if(NOT EXISTS ${ASHDESK_ENGINE_DIR}/adrd_client.app)
      message(FATAL_ERROR "ASHDESK_ENGINE_DIR 里没有 adrd_client.app：${ASHDESK_ENGINE_DIR}")
    endif()
    install(DIRECTORY ${ASHDESK_ENGINE_DIR}/adrd_client.app DESTINATION AshDesk.app/Contents/Helpers USE_SOURCE_PERMISSIONS)
    install(FILES packaging/macos/helper_qt.conf RENAME qt.conf DESTINATION AshDesk.app/Contents/Helpers/adrd_client.app/Contents/Resources)
    set(ASHDESK_DEPLOY_EXTRA "\"-executable=\${CMAKE_INSTALL_PREFIX}/AshDesk.app/Contents/Helpers/adrd_client.app/Contents/MacOS/adrd_client\"")
  endif()
  install(CODE "
    set(app \"\${CMAKE_INSTALL_PREFIX}/AshDesk.app\")
    execute_process(COMMAND \"${ASHDESK_MACDEPLOYQT}\" \"\${app}\" ${ASHDESK_DEPLOY_EXTRA} COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND codesign --force --deep --sign - \"\${app}\" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND codesign --verify --deep --strict \"\${app}\" COMMAND_ERROR_IS_FATAL ANY)")
elseif(UNIX)
  include(GNUInstallDirs)
  install(TARGETS ashdesk RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ui/icons/app/ashdesk.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  foreach(size 16 24 32 48 64 128 256)
    install(FILES ui/icons/app/ashdesk-${size}.png RENAME ashdesk.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps)
  endforeach()
endif()
target_link_libraries(ashdesk PRIVATE ashdesk_widgets)
target_compile_options(ashdesk PRIVATE ${ASHDESK_WARN})

if(ASHDESK_HAVE_GTEST)
  include(GoogleTest)
  find_package(Qt6 REQUIRED COMPONENTS Test)
  add_executable(ashdesk_ui_tests tests/gtest/desktop_ui_test.cpp tests/gtest/glass_benchmark_test.cpp)
  target_link_libraries(ashdesk_ui_tests PRIVATE ashdesk_widgets GTest::gtest Qt6::Test)
  target_compile_options(ashdesk_ui_tests PRIVATE ${ASHDESK_WARN})
  # 假核心：界面测试把它复制成 adrd_host / adrd_client，由真实的 MainWindow 拉起（见 tests/gtest/fake_core.cpp）
  add_executable(ashdesk_fake_core tests/gtest/fake_core.cpp)
  target_compile_options(ashdesk_fake_core PRIVATE ${ASHDESK_WARN})
  add_dependencies(ashdesk_ui_tests ashdesk_fake_core)
  if(WIN32)
    set(ASHDESK_UI_TEST_PLATFORM windows)
  else()
    set(ASHDESK_UI_TEST_PLATFORM offscreen)
  endif()
  gtest_discover_tests(ashdesk_ui_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${ASHDESK_UI_TEST_PLATFORM}")
else()
  message(WARNING "没有 GoogleTest：界面测试不编（取得方式见 third_party/googletest/README.AshDesk.md）")
endif()
