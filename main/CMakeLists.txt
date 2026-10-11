idf_component_register(
    SRCS "main.c"
         "whale_model.c"
         "whale_tone.c"
         "whale_audio.c"
         "whale_form.c"
         "whale_store.c"
         "whale_net.c"
         "whale_prov.c"
         "whale_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_hw_support esp_timer
             esp_wifi esp_netif esp_event esp_http_client esp_http_server
             esp-tls mbedtls
)

# 应用自带的中文字模(assets/fonts/whale_ui_font_16.c / whale_ui_font_24.c)。
# 它们是 LVGL 位图字体,单独编译进本组件;不要同时在 SRCS 里再列一次,否则符号重复定义。
# 角色图 / 气泡图(assets/img/whale_dsnang_img.c / whale_bubble_img.c)同理:
# 分别由 tools/gen_whale_img.py、tools/gen_whale_bubble.py 生成,单独编译。
# 注:另一个应用 main/g2048_*.c 仍留在工作区(同类改动已备份到
# E:/Workbuddy/MPD/game-2048-backup-20261010/),但不属于本应用的构建。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/whale_ui_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/whale_ui_font_24.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/img/whale_dsnang_img.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/img/whale_bubble_img.c"
)
