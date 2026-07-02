# BCMDHD SDIO Boot Driver Flow

本文只梳理 SDIO 相关路径，忽略 USB、PCIe 和 gSPI。目标是给 Windows 版本重写提供实现顺序和模块边界。函数名和文件名均对应当前 Linux bcmdhd 源码。

## 1. 分层结构

bcmdhd 的 SDIO 驱动可以按 5 层理解：

1. OS glue 层
   - 主要文件：`dhd_linux.c`
   - 负责模块加载、网络设备注册、`net_device` open/stop、线程、锁、wakelock、ifidx 管理。
   - Windows 重写时对应 NDIS/WDF 入口、miniport adapter 生命周期、OID/IOCTL glue。

2. 平台电源和 DTS/ACPI/registry 层
   - 主要文件：`dhd_linux_platdev.c`、`dhd_gpio.c`、`dhd_custom_gpio.c`
   - 负责解析 `android,bcmdhd_wlan`，拿到 `WL_REG_ON`、`WL_HOST_WAKE`、OOB IRQ、firmware/nvram 路径、bus/slot。
   - Windows 重写时建议抽象为 PlatformOps：PowerOn/PowerOff/BusEnumerate/GetOobIrq/GetFirmwarePaths。

3. BCMSDH host abstraction 层
   - 主要文件：`bcmsdh_linux.c`、`bcmsdh_sdmmc_linux.c`、`bcmsdh_sdmmc.c`、`bcmsdh.c`
   - 包装 Linux MMC/SDIO API，向上提供 `bcmsdh_cfg_read/write`、`bcmsdh_recv_buf/send_buf`、`bcmsdh_intr_*`、`bcmsdh_reg_read/write`。
   - Windows 重写时对应 SD bus interface abstraction，底层可能是 SDPORT/SDBUS 或私有 SDIO controller API。

4. DHD SDIO bus 层
   - 主要文件：`dhd_sdio.c`
   - 负责芯片识别、backplane/SI attach、firmware/NVRAM 下载、F2 enable、SDPCM 收发、OOB/SDIO interrupt DPC。
   - 这是 Windows 重写的核心。

5. 协议和 cfg80211/wl 层
   - 主要文件：`dhd_cdc.c`、`dhd_common.c`、`wl_cfg80211.c`、`wl_android.c`
   - `dhd_cdc.c` 把 ioctl/iovar 打包成 CDC control message，再通过 SDPCM control channel 走 SDIO。
   - Windows 重写时对应 OID、scan/connect/auth key offload、event indication。

## 2. 模块加载总流程

Linux 入口在 `dhd_linux.c`：

```text
dhd_module_init()
  -> _dhd_module_init()
     -> dhd_static_buf_init()                    // 可选，静态内存池
     -> dhd_wifi_platform_register_drv()
        -> dhd_wifi_platform_load()
           -> wl_android_init()
           -> dhd_wifi_platform_load_sdio()
              -> wifi_platform_set_power(TRUE)
              -> wifi_platform_bus_enumerate(TRUE)
              -> dhd_bus_register()
                 -> bcmsdh_register(&dhd_sdio)
                    -> bcmsdh_register_client_driver()
                       -> sdio_register_driver(&bcmsdh_sdmmc_driver)
```

关键点：

- `_dhd_module_init()` 有 `POWERUP_MAX_RETRY` 重试。
- 若启用静态内存，先初始化 DHD 静态 buffer。
- `dhd_wifi_platform_load()` 代码顺序是 USB -> SDIO -> PCIe，但本平台 SDIO 生效。
- `dhd_wifi_platform_load_sdio()` 在注册 SDIO client driver 前先上电并触发枚举。
- `dhd_registration_sem` 用于等待 `sdio_register_driver()` 触发 probe 并完成 attach。
- 如果 `dhd_wifi_platform_load_sdio()` 等不到 probe，会 power off、unregister、返回失败。

## 3. 平台电源和枚举

`dhd_wifi_platform_load_sdio()` 在 `dhd_linux_platdev.c` 中完成 SDIO 电源时序：

```text
dhd_bus_reg_sdio_notify(&dhd_chipup_sem)
wifi_platform_set_power(adapter, TRUE, WIFI_TURNON_DELAY)
wifi_platform_bus_enumerate(adapter, TRUE)
wait dhd_chipup_sem
dhd_bus_register()
wait dhd_registration_sem
```

典型平台数据来自 DTS：

```dts
wireless-wlan {
    compatible = "android,bcmdhd_wlan";
    gpio_wl_reg_on = <...>;
    gpio_wl_host_wake = <...>;
};
```

运行日志对应：

```text
Get GPIO from DTS(android,bcmdhd_wlan)
dhd_wlan_init_gpio: gpio_wl_host_wake=15, oob_irq=122
wifi_platform_set_power = 1
Card detection to detect SDIO card
```

Windows 重写建议：

- `WL_REG_ON` 必须可控或确认由 ACPI/regulator 固定拉高。
- `WL_HOST_WAKE` 必须作为 GPIO interrupt 输入，常见为 falling/low active，当前日志 `irq_flags=0x4`。
- PowerOn 后至少 delay 200 ms，再触发 SDIO card detect/rescan。
- 如果 SDIO 控制器不支持软件 rescan，PowerOn 必须发生在 SD stack 枚举前。

## 4. SDIO device match 和 probe

SDIO client driver 在 `bcmsdh_sdmmc_linux.c`：

```text
static const struct sdio_device_id bcmsdh_sdmmc_ids[] = {
    SDIO_DEVICE(0x02d0, 0x0000),
    SDIO_DEVICE(0x02d0, BCM4362_CHIP_ID),
    SDIO_DEVICE(0x02d0, BCM43751_CHIP_ID),
    SDIO_DEVICE(0x02d0, BCM43752_CHIP_ID),
    ...
};
```

Probe 路径：

```text
bcmsdh_sdmmc_probe(func, id)
  -> only func 2, or special func 1/device 0x4
  -> sdioh_probe(func)
     -> get mmc host index and card rca
     -> dhd_wifi_platform_get_adapter(SDIO_BUS, host_idx, rca)
     -> osl_attach()
     -> sdioh_attach()
     -> ensure NONREMOVABLE / no polling if SDIO_DETECT_CHANGE
     -> bcmsdh_probe(osh, &func->dev, sdioh, adapter, SDIO_BUS, host_idx, rca)
     -> sdio_set_drvdata(func, sdioh)
```

`bcmsdh_probe()` 会调用上层注册的 `dhd_sdio.probe`，也就是 `dhdsdio_probe()`。

Windows 重写建议：

- 只绑定 SDIO function 2 作为数据功能；function 1 用于 backplane/config register 访问。
- 要缓存 host/bus/slot/function 对象，后续 `send_buf/recv_buf/cfg_read/cfg_write` 全部依赖它。
- 需要设置 F2 block size，当前日志为 `set sd_f2_blocksize 256`。
- 初始枚举时 SDIO vendor/product 可能是 `0x02d0/0xaae7`，芯片内部 SI ID 是 `0xaae8 rev 2`。

## 5. bcmsdh 到 dhd_sdio 的连接

`dhd_sdio.c` 中注册的 bus driver：

```text
static bcmsdh_driver_t dhd_sdio = {
    dhdsdio_probe,
    dhdsdio_disconnect,
    dhdsdio_suspend,
    dhdsdio_resume,
    ...
};

dhd_bus_register()
  -> bcmsdh_register(&dhd_sdio)
```

`bcmsdh_register()` 将 `dhd_sdio` 存入全局 `drvinfo`，随后注册底层 SDIO client driver。底层发现设备后，`bcmsdh_probe()` 再通过 `drvinfo.attach/probe` 调到 `dhdsdio_probe()`。

## 6. dhdsdio_probe：核心 attach 阶段

`dhdsdio_probe()` 是 SDIO boot 的主入口：

```text
dhdsdio_probe(venid, devid, bus_no, slot, func, bustype, regsva, osh, sdh)
  -> init runtime globals
  -> validate vendor/device
  -> allocate dhd_bus_t
  -> dhdsdio_probe_attach()
  -> dhd_attach()
  -> dhd_conf_get_otp()
  -> dhdsdio_probe_malloc()
  -> dhdsdio_probe_init()
  -> bcmsdh_intr_reg(sdh, dhdsdio_isr, bus) if bus->intr
  -> dhd_bus_start(bus->dhd) if firmware should load at insmod
  -> dhd_attach_net()
  -> return bus
```

### 6.1 dhd_bus_t 初始化

重要字段：

- `bus->sdh`：bcmsdh/sdioh context。
- `bus->cl_devid`：缓存 SDIO device id，power cycle 后 reattach 用。
- `bus->bus_num`、`bus->slot_num`：平台匹配用。
- `bus->tx_seq = SDPCM_SEQUENCE_WRAP - 1`：TX SDPCM sequence 初始值。
- `bus->rx_seq`：RX 期望 sequence，后续读包校验。
- `bus->txq`：host 待发数据队列。
- `bus->ctrl_tx_wait`：control packet 等待队列。

Windows 重写建议把它拆成：

- `AdapterContext`
- `SdioBusContext`
- `FirmwareContext`
- `SdpcmState`
- `PowerInterruptState`

## 7. dhdsdio_probe_attach：识别芯片和 backplane

`dhdsdio_probe_attach()` 完成芯片低层初始化：

```text
dhdsdio_set_siaddr_window(si_enum_base(devid))
bcmsdh_cfg_write(F1, CHIPCLKCSR, DHD_INIT_CLKCTL1)
bcmsdh_cfg_read(F1, CHIPCLKCSR)
si_attach()
bcmsdh_chipinfo()
dhdsdio_chipmatch()
dhdsdio_clk_kso_init()
dhdsdio_set_wakeupctrl()
si_sdiod_drive_strength_init()
dhd_bus_sdio_pwr_req_nolock()
find ARM core
find SYSMEM/SOCRAM/TCM size
select dongle_ram_base
find SDIOD/PCMCIA core regs
enable CC_BPRESEN
init txq, rxhdr, interrupt/poll flags
```

关键寄存器/概念：

- F0 CCCR：标准 SDIO config space。
- F1：Broadcom backplane/window/config function。
- F2：frame data transfer function。
- `SBSDIO_FUNC1_CHIPCLKCSR`：ALP/HT clock 请求和状态。
- `si_enum_base(devid)`：backplane enumeration base。
- `si_attach()`：扫描 Broadcom internal cores。
- `PCMCIA_CORE_ID` / `SDIOD_CORE_ID`：SDPCM register core。
- `bus->regs`：SDPCM core register base。

芯片识别日志示例：

```text
F1 signature read @0x18000000=0x1042aae8
F1 signature OK, socitype:0x1 chip:0xaae8 rev:0x2 pkg:0x4
DHD: dongle ram size is set to 1310720(orig 1310720) at 0x170000
```

Windows 重写要实现的最小能力：

- F1 config byte read/write。
- F1 backplane register window read/write。
- 根据 chip/rev 找 RAM base 和 RAM size。
- 找 SDIOD core registers。
- KSO/wakeup control。
- drive strength 可先固定默认值，但要预留配置。

## 8. dhdsdio_probe_malloc：运行 buffer

`dhdsdio_probe_malloc()` 分配：

- `rxbuf`：control frame 接收 buffer，大小约 `maxctl + SDPCM_HDRLEN + align`。
- `databuf`/`dataptr`：大数据或 glom 接收 buffer。
- `membuf`：firmware/NVRAM 下载和 backplane memory 操作用临时 buffer。

Windows 重写建议：

- SDIO DMA buffer 必须满足控制器 alignment 要求。
- 保留 `DHD_SDALIGN` 对齐。
- F2 data transfer buffer 要支持 packet chain 或 fallback copy。

## 9. dhdsdio_probe_init：初始 SDPCM/SDIO 状态

`dhdsdio_probe_init()`：

```text
bcmsdh_cfg_write(F0, IOEN, enable F1 only)
busstate = DHD_BUS_DOWN
clock -> SDONLY
query iovar sd_divisor
query iovar sd_mode
query iovar sd_blocksize for F2
bus->blocksize = F2 block size
bus->roundup = min(max_roundup, blocksize)
query sd_rxchain
bus->dotxinrx = TRUE
```

此时 F2 通常还没有正式 ready，固件也未必已下载。

## 10. 固件下载总流程

固件下载通常由 `dhd_bus_start()` 或 `dhd_bus_devreset(FALSE)` 间接触发，核心函数：

```text
dhd_bus_download_firmware(bus, osh, fw_path, nv_path)
  -> dhdsdio_download_firmware()
     -> dhd_conf_set_path_params()
     -> dhd_set_bus_params()
     -> dhdsdio_clkctl(CLK_AVAIL)
     -> _dhdsdio_download_firmware()
     -> dhdsdio_clkctl(CLK_SDONLY)
```

`_dhdsdio_download_firmware()`：

```text
dhdsdio_download_state(TRUE)       // hold ARM in reset
dhdsdio_download_code_file(fw)
dhdsdio_download_nvram()
dhdsdio_download_state(FALSE)      // release ARM
```

### 10.1 固件路径选择

路径由 `dhd_conf_set_path_params()` 和平台配置决定，当前实机日志：

```text
Final fw_path=/lib/firmware/fw_bcm4359c51a2_ag.bin
Final nv_path=/lib/firmware/nvram_ap6398sv.txt
Final clm_path=/lib/firmware/clm_bcm4359c51a2_ag.blob
```

当前 AP6398SV/43752 实机固件：

```text
FW: 18.35.387.23.146
NVRAM: AP6398SV_NVRAM_V1.2_20210531
CLM: 9.9.10_SS
```

### 10.2 dhdsdio_download_code_file

流程：

```text
open firmware image
bus->fw_download_len = firmware size
bus->fw_download_addr = bus->dongle_ram_base
allocate aligned memblock
if CR4/CA7, save reset instruction at image offset 0
for each block:
    read file chunk
    dhdsdio_membytes(write, dongle_ram_base + offset, chunk)
optional readback compare
close image
```

重点：

- 写入目标不是 SDIO F2 FIFO，而是通过 F1/backplane window 写 dongle RAM。
- CR4/CA7 固件 offset 0 的 reset instruction 会被特殊处理。
- 下载块大小受 `MEMBLOCK`、`MAX_MEMBLOCK`、F1 block size 影响。

### 10.3 dhdsdio_download_nvram

流程：

```text
try get NVRAM from UEFI/download buffer
else open nvram text file
process_nvram_vars()
4-byte align
append final NUL
dhdsdio_downloadvars(bus, nvram, len + 1)
```

NVRAM 写到 dongle RAM 尾部共享区域，firmware 启动后读取这些 key/value。

Windows 重写注意：

- NVRAM 文本需要 Broadcom 格式处理：注释/换行、变量压缩、末尾 NUL、4 字节对齐。
- 固件和 NVRAM 必须匹配模组，否则可能枚举成功但射频异常。

### 10.4 CLM blob

CLM 不是在 `_dhdsdio_download_firmware()` 中写入，而是在 firmware up 后通过 ioctl/iovar 下载：

```text
dhd_apply_default_clm()
```

日志：

```text
dhd_check_current_clm_data: This FW is not included CLM data
dhd_apply_default_clm: CLM download succeeded
dhd_check_current_clm_data: This FW is included CLM data
```

Windows 重写中可在 firmware ready 后，用 vendor iovar 下载 CLM。

## 11. dhd_bus_init：启动 F2 数据通道

固件 release 后，需要打开 F2：

```text
dhd_bus_init(dhdp, enforce_mutex)
  -> dhdsdio_clkctl(CLK_AVAIL)
  -> read F1 CHIPCLKCSR
  -> force HT clock
  -> write SDPCM_PROT_VERSION to tosbmailboxdata
  -> enable F1|F2 via F0 IOEN
  -> poll F0 IORDY until F1|F2 ready
  -> select SDIOD/PCMCIA core
  -> hostintmask = HOSTINTMASK
  -> write hostintmask
  -> optional watermark
  -> busstate = DHD_BUS_DATA
  -> enable SDIO/OOB interrupt if configured
```

成功日志：

```text
dhd_bus_init: enable 0x06, ready 0x06
Firmware up: op_mode=0x0005, MAC=...
```

关键状态：

- `DHD_BUS_DOWN`：不能收发。
- `DHD_BUS_DATA`：F2 ready，可收发 SDPCM data/control/event。
- `DHD_BUS_SUSPEND`：suspend 中。

Windows 重写注意：

- F2 未 ready 时不要开始传输。
- `IOEN=0x06` 表示 enable F1 + F2。
- `IORDY` 必须等到 `0x06`。
- `hostintmask` 决定 firmware 哪些中断会通知 host。

## 12. OOB/SDIO 中断路径

本平台启用 OOB host wake。注册路径：

```text
dhd_bus_init()
  -> bcmsdh_oob_intr_register(sdh, dhdsdio_isr, bus)
     -> request_irq(oob_irq_num, wlan_oob_irq, flags, "bcmsdh_sdmmc", bcmsdh)
```

中断流程：

```text
WL_HOST_WAKE GPIO interrupt
  -> wlan_oob_irq()
     -> disable host irq
     -> bcmsdh_osinfo->oob_irq_handler(context)
        -> dhdsdio_isr(bus)
           -> bus->ipend = TRUE
           -> disable SDIO/OOB interrupt
           -> schedule DPC
              -> dhdsdio_dpc(bus)
```

`dhdsdio_isr()` 不做重活，只标记 `ipend`，关中断，调度 DPC。

`dhdsdio_dpc()` 负责：

- 打开 clock。
- 读取 SDPCM core `intstatus`。
- ACK 中断状态。
- 处理 mailbox。
- 读 RX frame。
- 发送 TX queue。
- 重新 enable interrupt。
- 判断是否需要 reschedule。

Windows 重写建议：

- ISR 只做最小工作，DPC/work item 做 SDIO transaction。
- level trigger OOB 要在 DPC 处理结束后再 unmask。
- edge trigger OOB 需要防丢中断：重新 enable 后再读一次 dongle intstatus。

## 13. SDPCM 帧格式

所有 F2 数据使用 SDPCM frame。TX preprocess 中注释给出格式：

```text
4-byte hardware frame tag:
    u16 length
    u16 ~length

optional 8-byte hardware extension, for tx glom:
    u16 packet length without HW tag/padding
    u16 channel/flags
    u16 header length
    u16 tail padding

8-byte software frame tag:
    u32 flags0: tx sequence, channel, data offset, flow-control/window fields
    u32 flags1: reserved/extension
```

常用 channel：

- `SDPCM_CONTROL_CHANNEL`：CDC ioctl/iovar request/response。
- `SDPCM_DATA_CHANNEL`：802.3 data packet。
- `SDPCM_EVENT_CHANNEL`：firmware event。
- `SDPCM_GLOM_CHANNEL`：RX/TX 聚合描述或 superframe。
- `SDPCM_TEST_CHANNEL`：测试。

长度校验：

```text
check is valid when ~(len ^ check) == 0
```

Sequence：

- Host TX 用 `bus->tx_seq`，每成功发送一个 SDPCM frame 递增。
- Host RX 用 `bus->rx_seq`，每成功收一个 frame 递增。
- `txmax` 是 dongle 在 RX header 里返回的 host TX window。

## 14. TX 数据路径

网络栈数据发送：

```text
ndo_start_xmit / DHD TX
  -> dhd_bus_txdata(bus, pkt)
     -> dhd_prot_hdrpush()         // 加 CDC/WLFC 等 DHD protocol header
     -> enqueue bus->txq or direct send
     -> dhdsdio_sendfromq()
        -> dhdsdio_txpkt(SDPCM_DATA_CHANNEL)
```

`dhdsdio_txpkt()`：

```text
for each packet:
    dhdsdio_txpkt_preprocess()
        -> push SDPCM header
        -> align head/tail to DHD_SDALIGN
        -> optionally allocate new aligned packet
        -> fill hardware len/check
        -> fill optional tx-glom extension
        -> fill software header: channel + seq + data offset
chain packets if glom
dhd_bcmsdh_send_buf(..., SDIO_FUNC_2, F2SYNC, ...)
if OK:
    bus->tx_seq += num_pkt
dhdsdio_txpkt_postprocess()
free/complete original packets
```

关键点：

- 真正写 SDIO F2 的函数是 `bcmsdh_send_buf()`。
- 地址通常是 `bcmsdh_cur_sbwad(sdh)`。
- 单包和 packet chain 都支持。
- 写入长度要满足 SDIO block/alignment 要求。
- 若 `fcstate` 或 `flowcontrol` 阻塞，对应 priority 的队列不能发。

Windows 重写建议：

- TX queue 与 DPC 串行化，避免和 RX/control 互相打断 SDIO transaction。
- `tx_seq` 只在 F2 write 成功后更新。
- 需要处理 headroom 不足时复制到 aligned bounce buffer。

## 15. Control path：ioctl/iovar

`dhd_cdc.c` 使用 SDPCM control channel：

```text
dhdcdc_msg()
  -> dhd_bus_txctl(bus, msg, len)
  -> wait response
  -> dhd_bus_rxctl(bus, msg, len)
```

`dhd_bus_txctl()` 把 control message 通过 `SDPCM_CONTROL_CHANNEL` 发给 firmware。`dhd_bus_rxctl()` 等待 `dhdsdio_read_control()` 填充 `bus->rxctl/rxlen`。

用途：

- firmware preinit ioctls。
- country code。
- CLM 下载。
- scan/connect/key 等 wl iovar。
- 获取 firmware version/capability。

Windows 重写建议：

- Control request 是同步语义，但底层 response 由 RX/DPC 收到。
- 需要 `ctrl_tx_wait`/completion/event。
- 需要超时和 bus down 错误返回。

## 16. RX 路径

DPC 中读包：

```text
dhdsdio_dpc()
  -> if PKT_AVAILABLE(intstatus)
     -> dhdsdio_readframes(bus, rxlimit, &rxdone)
```

`dhdsdio_readframes()`：

```text
loop until rxlimit or no frame:
    if glom pending:
        dhdsdio_rxglom()
        continue

    if readahead nextlen exists:
        read full frame from F2
    else:
        read firstread header from F2
        parse len/check/channel/seq/doff/txmax/nextlen
        if control:
            dhdsdio_read_control()
            continue
        allocate packet
        read remaining frame body from F2

    validate:
        len/check
        doff >= SDPCM_HDRLEN
        sequence
        txmax sanity
        flow-control bits

    if DATA/EVENT:
        trim SDPCM header
        dhd_prot_hdrpull()
        dhd_rx_frame(dhd, ifidx, pkt, pkt_count, chan)
```

`nextlen`：

- Dongle 在 SDPCM header 中提示下一帧长度，单位通常为 16 bytes。
- Host 下次可一次性读完整帧，减少一次 header read。
- 如果 `nextlen` 和实际 header len 不一致，需要 abort/retry。

错误处理：

- HW header 校验失败：`dhdsdio_rxfail()`。
- Control frame 失败一般要求 retransmit。
- 数据包内存不足时可丢弃，event/control 通常要求 retry。
- 严重 backplane/SDIO 失败会置 `DHD_BUS_DOWN`。

## 17. RX glom

`dhdsdio_rxglom()` 处理聚合包：

```text
if bus->glomd:
    parse descriptor, each u16 sublen
    allocate packet chain
read superframe from F2
validate superframe header
for each subframe:
    validate len/check/channel/doff
    trim SDPCM header
    dhd_prot_hdrpull()
    append to ifidx list
dhd_rx_frame()
```

重写时可以先实现非 glom，确认基本联网后再实现 glom。但现代固件默认可能启用 glom，若不支持需要在 preinit iovar 中禁用或限制。

## 18. Mailbox 和 firmware ready

DPC 中处理 `I_HMB_HOST_INT`：

```text
dhdsdio_hostmail()
  -> read tohostmailboxdata
  -> ack SMB_INT_ACK
  -> if HMB_DATA_DEVREADY/FWREADY:
        check SDPCM version
        read shared console addr
  -> if HMB_DATA_NAKHANDLED:
        rxskip = FALSE; expect retransmit
  -> if HMB_DATA_FC:
        update flowcontrol
  -> if HMB_DATA_FWHALT:
        firmware halted
```

Windows 重写要关注：

- Firmware ready 后才能进行 preinit ioctl。
- SDPCM protocol version 必须匹配 `SDPCM_PROT_VERSION`。
- FWHALT 要触发 adapter reset/recovery。

## 19. dhd_open：用户启用 wlan0 后的 power-on

如果配置为 `dhd_download_fw_on_driverload = false`，模块加载只注册 netdev，不下载 firmware。真正下载发生在 interface open：

```text
dhd_open(net)
  -> wl_android_wifi_on(net)
     -> wifi_platform_set_power(TRUE)
     -> sdio_sw_reset / mmc reset
     -> dhd_bus_devreset(FALSE)
        -> bcmsdh_reset()
        -> dhdsdio_probe_attach()
        -> dhdsdio_probe_init()
        -> dhdsdio_download_firmware()
        -> dhd_bus_init()
        -> bcmsdh_oob_intr_register()
        -> dhd->up = TRUE
        -> start watchdog
```

当前实机日志就是这种路径：

```text
module init -> Register interface wlan0 -> wifi off
NetworkManager opens wlan0
dhd_open
wl_android_wifi_on
Firmware up
wlan0 dhd_open Exit ret=0
```

## 20. dhd_bus_devreset：power off/on

Power off：

```text
dhd_bus_devreset(TRUE)
  -> dhd_bus_stop()
  -> unregister OOB IRQ
  -> dhdsdio_release_dongle()
  -> dongle_reset = TRUE
  -> up = FALSE
  -> busstate = DHD_BUS_DOWN
```

Power on：

```text
dhd_bus_devreset(FALSE)
  -> bcmsdh_reset()
  -> dhdsdio_probe_attach()
  -> dhdsdio_probe_init()
  -> dhdsdio_download_firmware()
  -> dhd_bus_init()
  -> register OOB IRQ
  -> dongle_reset = FALSE
  -> up = TRUE
```

Windows 重写建议：

- Adapter reset path 复用 power on 流程。
- Reset 必须清理 IRQ、TX queue、control wait、busstate。
- 如果 SDIO transaction 失败，应进入 bus down 并由上层触发 full reset。

## 21. Suspend/Resume

SDIO suspend/resume 入口：

```text
dhdsdio_suspend()
  -> flowcontrol ON
  -> busstate = DHD_BUS_SUSPEND
  -> wait idle/sleep

dhdsdio_resume()
  -> enable OOB IRQ if interface up
  -> busstate = DHD_BUS_DATA
  -> flowcontrol OFF
```

Windows 重写中对应 D0/Dx、电源策略、WoWLAN。基础联网可先实现 D0 only，后续再补 WoWLAN。

## 22. 最小 Windows 重写里程碑

建议按以下顺序实现：

1. SDIO transport
   - F0 config read/write。
   - F1 config read/write。
   - F1 backplane register/memory read/write with window。
   - F2 block read/write。
   - block size、alignment、timeout、abort。

2. Platform power
   - WL_REG_ON。
   - SDIO rescan/enumerate。
   - HOST_WAKE interrupt。

3. Chip attach
   - read signature。
   - SI core scan。
   - chip/rev match。
   - RAM size/base。
   - SDIOD core regs。

4. Firmware boot
   - hold ARM reset。
   - write firmware image to RAM。
   - process and write NVRAM。
   - release ARM reset。
   - wait firmware ready/mailbox。

5. F2/SDPCM data path
   - enable F2。
   - hostintmask。
   - ISR/DPC。
   - SDPCM TX control。
   - SDPCM RX control。

6. CDC/iovar
   - synchronous request/response。
   - firmware version。
   - preinit ioctls。
   - CLM download。

7. Data/event path
   - TX Ethernet frame。
   - RX Ethernet frame。
   - firmware events。
   - scan/connect/key handling。

8. Robustness
   - glom。
   - flow control。
   - tx window。
   - suspend/resume。
   - firmware halt/reset recovery。

## 23. 当前平台 AP6398SV/43752 验证点

从实机日志看，当前正常路径应出现：

```text
F1 signature read @0x18000000=0x1042aae8
F1 signature OK, socitype:0x1 chip:0xaae8 rev:0x2 pkg:0x4
dhd_conf_set_chiprev : devid=0xaae7, chip=0xaae8, chiprev=2
Register interface [wlan0]
fw_path=/lib/firmware/fw_bcm4359c51a2_ag.bin
nv_path=/lib/firmware/nvram_ap6398sv.txt
clm_path=/lib/firmware/clm_bcm4359c51a2_ag.blob
dhdsdio_write_vars: Download, Upload and compare of NVRAM succeeded
dhd_bus_init: enable 0x06, ready 0x06
bcmsdh_oob_intr_register: HW_OOB irq=122 flags=0x4
Firmware up
wlan0 dhd_open : Exit ret=0
Link UP
connection succeeded
```

非致命日志：

```text
FILS NOT supported
roam_rssi_limit failed -23
failed to start ecounters
error get bw_cap 6g (-13)
sroam config failed -23
```

需要注意的硬件/DTS 问题：

```text
dhd_wlan_request_gpio: gpio_request(20) for WL_REG_ON failed -16
```

`-16` 是 busy，说明 WL_REG_ON GPIO 被别处占用。当前 WiFi 可用，说明电源脚已被其它节点或固定上拉控制；但正式设计中应避免重复申请。

## 24. Windows 实现时的核心状态机

建议抽象如下状态：

```text
AdapterCreated
  -> PlatformPowerOn
  -> SdioEnumerated
  -> ChipAttached
  -> FirmwareDownloaded
  -> F2Ready
  -> FirmwareReady
  -> NetIfReady
  -> Connected
```

错误恢复：

```text
Any SDIO fatal error
  -> BusDown
  -> DisableInterrupt
  -> StopTxRx
  -> PowerCycle or SdioReset
  -> ChipAttached
  -> FirmwareDownloaded
  -> F2Ready
```

关键不变量：

- `DHD_BUS_DATA` 前不得发 F2 data/control。
- ISR 不直接做 SDIO bulk read/write。
- F2 write 成功后才更新 `tx_seq`。
- RX header len/check 不通过必须 abort 或 retry。
- control request 必须有超时。
- HOST_WAKE interrupt unmask 应发生在 DPC 完成后。
- firmware/NVRAM/CLM 必须同模组匹配。
