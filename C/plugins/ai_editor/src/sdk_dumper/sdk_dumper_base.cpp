// sdk_dumper_base.cpp — factory + shared helpers.

#include "sdk_dumper_base.h"

namespace sao::ai_editor::sdk_dumper {

DumperBase* create_il2cpp();
DumperBase* create_mono();
DumperBase* create_unreal();
DumperBase* create_source();

DumperBase* create_dumper(DumpKind kind) {
    switch (kind) {
    case DumpKind::IL2CPP: return create_il2cpp();
    case DumpKind::Mono:   return create_mono();
    case DumpKind::Unreal: return create_unreal();
    case DumpKind::Source: return create_source();
    }
    return nullptr;
}

} // namespace sao::ai_editor::sdk_dumper
