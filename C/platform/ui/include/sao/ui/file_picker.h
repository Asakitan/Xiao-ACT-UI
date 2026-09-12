#pragma once
#include "sao/ui/compositor.h"

extern "C" {
typedef struct sao_ui_file_picker_s* sao_ui_file_picker_handle_t;
enum SaoUiFilePickerMode : int32_t {
	SAO_UI_FILE_PICKER_OPEN = 0,
	SAO_UI_FILE_PICKER_SAVE = 1,
	SAO_UI_FILE_PICKER_FOLDER = 2
};
struct SaoUiFilePickerConfig {
	const char* title_utf8;
	const char* initial_path_utf8;
	const char* filter_utf8;
	int32_t mode;
};
typedef void(SAO_UI_CALL* sao_ui_file_picker_result_fn_t)(sao_status_t, const char*, void*);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_create(sao_ui_compositor_handle_t, sao_ui_file_picker_handle_t*);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_show(sao_ui_file_picker_handle_t, const SaoUiFilePickerConfig*, sao_ui_file_picker_result_fn_t, void*);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_cancel(sao_ui_file_picker_handle_t);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_try_destroy(sao_ui_file_picker_handle_t);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_file_picker_is_visible(sao_ui_file_picker_handle_t, bool*);
}
