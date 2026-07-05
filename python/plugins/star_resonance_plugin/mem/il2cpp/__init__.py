# IL2CPP 静态分析子包.
#
# 由于腾讯 ACE 把 global-metadata.dat 在磁盘上掏空 (0 字节), 必须先用 mem_dump_metadata
# 从运行中的 Star.exe 把解密后的 metadata 抓出来, 再用 Il2CppDumper 离线生成 dump.cs/script.json,
# 最后由 metadata_builder 转成自定义 metadata.json 供 resolver 使用。
