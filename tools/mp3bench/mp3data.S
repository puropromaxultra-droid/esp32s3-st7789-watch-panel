/* mp3bench 测试素材：以 incbin 方式嵌入，避免文件系统 / SD 读干扰计时。
 * 数据本体在 data/ 下，由 tools/_mkmp3.py 生成。
 *
 * ★ 路径为什么指向 C:/mp3b_data/：
 *   gas 的 .incbin 在 Windows 上打不开含中文的路径（项目目录名为「电子屏显示」），
 *   报 "file not found"。素材由 tools/_sync_bench_data.py 同步到 ASCII 路径后再编译。
 */
.section .rodata.mp3bench, "a", @progbits

.balign 4
.global mp3_a_start
mp3_a_start:
.incbin "C:/mp3b_data/a_44k_128_st.mp3"
.global mp3_a_end
mp3_a_end:

.balign 4
.global mp3_b_start
mp3_b_start:
.incbin "C:/mp3b_data/b_44k_320_st.mp3"
.global mp3_b_end
mp3_b_end:

.balign 4
.global mp3_c_start
mp3_c_start:
.incbin "C:/mp3b_data/c_48k_128_st.mp3"
.global mp3_c_end
mp3_c_end:

.balign 4
.global mp3_d_start
mp3_d_start:
.incbin "C:/mp3b_data/d_44k_128_mono.mp3"
.global mp3_d_end
mp3_d_end:

.balign 4
