#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
@file    gen_keil_project.py
@brief   生成 Keil MDK-ARM (uVision 5) 工程文件 firmware/MDK-ARM/QR_LVGL_F407.uvprojx。
@details
          手工维护 500 多个源文件的工程 XML 不现实，因此用本脚本扫描源码树并生成工程：
          - Core/Src/*.c            -> 分组 "Application"
          - startup_stm32f407xx.s   -> 分组 "Startup"
          - system_stm32f4xx.c      -> 分组 "CMSIS"
          - HAL 驱动 Src/*.c        -> 分组 "HAL Drivers"
          - qrcodegen.c             -> 分组 "QR Code"
          - LVGL src/**/*.c         -> 分组 "LVGL"

          目标器件：STM32F407VETx（512 KiB Flash / 128 KiB SRAM + 64 KiB CCM），
          ARM Compiler 6，C11，-O2，使用 Target 对话框的内存布局（无需 scatter 文件）。

@usage   python tools/gen_keil_project.py
@note    修改源码文件名后重新运行本脚本即可，不需要手改 .uvprojx。
"""

from __future__ import annotations

import os
from pathlib import Path

# ---------------------------------------------------------------------------
# 路径
# ---------------------------------------------------------------------------
TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent
FW_DIR = REPO_ROOT / "firmware"
MDK_DIR = FW_DIR / "MDK-ARM"
OUT_FILE = MDK_DIR / "QR_LVGL_F407.uvprojx"

# ---------------------------------------------------------------------------
# 工程参数
# ---------------------------------------------------------------------------
TARGET_NAME = "QR_LVGL_F407"
DEVICE = "STM32F407VE"
VENDOR = "STMicroelectronics"
PACK_ID = "Keil.STM32F4xx_DFP.2.17.0"

# IROM 512K, IRAM 128K, IRAM2(CCM) 64K
CPU_STRING = (
    'IRAM(0x20000000,0x20000) IRAM2(0x10000000,0x10000) '
    'IROM(0x8000000,0x80000) CPUTYPE("Cortex-M4") FPU2 '
    'CLOCK(12000000) ELITTLE'
)

DEFINES = "USE_HAL_DRIVER,STM32F407xx,LV_CONF_INCLUDE_SIMPLE"

INCLUDE_DIRS = [
    r"..\Core\Inc",
    r"..\Drivers\CMSIS\Include",
    r"..\Drivers\CMSIS\Device\ST\STM32F4xx\Include",
    r"..\Drivers\STM32F4xx_HAL_Driver\Inc",
    r"..\Middlewares\lvgl",
    r"..\Middlewares\qrcodegen",
]


def rel(path: Path) -> str:
    """把绝对路径转换成相对 MDK-ARM 目录的 Windows 反斜杠路径。"""
    return os.path.relpath(path, MDK_DIR).replace("/", "\\")


def collect() -> list[tuple[str, list[tuple[str, Path]]]]:
    """收集所有需要加入工程的源文件，按分组返回。"""
    groups: list[tuple[str, list[tuple[str, Path]]]] = []

    # 1. 应用层
    app_files = sorted((FW_DIR / "Core" / "Src").glob("*.c"))
    groups.append(("Application", [(p.name, p) for p in app_files]))

    # 2. 启动文件（汇编）
    startup = (
        FW_DIR
        / "Drivers"
        / "CMSIS"
        / "Device"
        / "ST"
        / "STM32F4xx"
        / "Source"
        / "Templates"
        / "arm"
        / "startup_stm32f407xx.s"
    )
    if startup.exists():
        groups.append(("Startup", [(startup.name, startup)]))

    # 3. CMSIS system 文件
    sys_src = (
        FW_DIR
        / "Drivers"
        / "CMSIS"
        / "Device"
        / "ST"
        / "STM32F4xx"
        / "Source"
        / "Templates"
        / "system_stm32f4xx.c"
    )
    if sys_src.exists():
        groups.append(("CMSIS", [(sys_src.name, sys_src)]))

    # 4. HAL 驱动
    hal_files = sorted((FW_DIR / "Drivers" / "STM32F4xx_HAL_Driver" / "Src").glob("*.c"))
    groups.append(("HAL Drivers", [(p.name, p) for p in hal_files]))

    # 5. qrcodegen
    qr_files = sorted((FW_DIR / "Middlewares" / "qrcodegen").glob("*.c"))
    groups.append(("QR Code", [(p.name, p) for p in qr_files]))

    # 6. LVGL（全部 .c，未启用的模块会被 lv_conf.h 内的宏编译成空文件）
    lvgl_src = FW_DIR / "Middlewares" / "lvgl" / "src"
    lvgl_files = sorted(lvgl_src.rglob("*.c"))
    groups.append(("LVGL", [(p.name, p) for p in lvgl_files]))

    return groups


def file_xml(name: str, path: Path) -> str:
    """生成单个 <File> 节点。"""
    ftype = 2 if path.suffix.lower() == ".s" else 1
    return (
        "          <File>\n"
        f"            <FileName>{name}</FileName>\n"
        f"            <FileType>{ftype}</FileType>\n"
        f"            <FilePath>{rel(path)}</FilePath>\n"
        "          </File>\n"
    )


def group_xml(name: str, files: list[tuple[str, Path]]) -> str:
    """生成一个 <Group> 节点。"""
    body = "".join(file_xml(n, p) for n, p in files)
    return (
        "        <Group>\n"
        f"          <GroupName>{name}</GroupName>\n"
        f"          <Files>\n{body}          </Files>\n"
        "        </Group>\n"
    )


TEMPLATE = """<?xml version="1.0" encoding="UTF-8" standalone="no" ?>
<Project xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="project_projx.xsd">

  <SchemaVersion>2.1</SchemaVersion>

  <Header>### uVision Project, (C) Keil Software</Header>

  <Targets>
    <Target>
      <TargetName>{target_name}</TargetName>
      <ToolsetNumber>0x4</ToolsetNumber>
      <ToolsetName>ARM-ADS</ToolsetName>
      <pCCUsed>6070000::V6.7::ARMCLANG</pCCUsed>
      <uAC6>1</uAC6>
      <TargetOption>
        <TargetCommonOption>
          <Device>{device}</Device>
          <Vendor>{vendor}</Vendor>
          <PackID>{pack_id}</PackID>
          <PackURL>https://www.keil.com/pack/</PackURL>
          <Cpu>{cpu}</Cpu>
          <FlashUtilSpec></FlashUtilSpec>
          <StartupFile></StartupFile>
          <FlashDriverDll>UL2CM3(-S0 -C0 -P0 -FD20000000 -FC1000 -FN1 -FF0STM32F4xx_512 -FS08000000 -FL080000 -FP0($$Device:STM32F407VE$CMSIS\\Flash\\STM32F4xx_512.FLM))</FlashDriverDll>
          <DeviceId>0</DeviceId>
          <RegisterFile>$$Device:STM32F407VE$Drivers\\CMSIS\\Device\\ST\\STM32F4xx\\Include\\stm32f407xx.h</RegisterFile>
          <MemoryEnv></MemoryEnv>
          <Cmp></Cmp>
          <Asm></Asm>
          <Linker></Linker>
          <OHString></OHString>
          <InfinionOptionDll></InfinionOptionDll>
          <SLE66CMisc></SLE66CMisc>
          <SLE66AMisc></SLE66AMisc>
          <SLE66LinkerMisc></SLE66LinkerMisc>
          <SFDFile>$$Device:STM32F407VE$CMSIS\\SVD\\STM32F407.svd</SFDFile>
          <bCustSvd>0</bCustSvd>
          <UseEnv>0</UseEnv>
          <BinPath></BinPath>
          <IncludePath></IncludePath>
          <LibPath></LibPath>
          <RegisterFilePath></RegisterFilePath>
          <DBRegisterFilePath></DBRegisterFilePath>
          <TargetStatus>
            <Error>0</Error>
            <ExitCodeStart>0</ExitCodeStart>
            <ExitCodeStop>0</ExitCodeStop>
            <ButtonStop>0</ButtonStop>
            <NotGenerated>0</NotGenerated>
            <Invalid>0</Invalid>
          </TargetStatus>
          <OutputDirectory>.\\Objects\\</OutputDirectory>
          <OutputName>{target_name}</OutputName>
          <CreateExecutable>1</CreateExecutable>
          <CreateLib>0</CreateLib>
          <CreateHexFile>1</CreateHexFile>
          <DebugInformation>1</DebugInformation>
          <BrowseInformation>0</BrowseInformation>
          <ListingPath>.\\Listings\\</ListingPath>
          <HexFormatSelection>1</HexFormatSelection>
          <Merge32K>0</Merge32K>
          <CreateBatchFile>0</CreateBatchFile>
          <BeforeCompile>
            <RunUserProg1>0</RunUserProg1>
            <RunUserProg2>0</RunUserProg2>
            <UserProg1Name></UserProg1Name>
            <UserProg2Name></UserProg2Name>
            <UserProg1Dos16Mode>0</UserProg1Dos16Mode>
            <UserProg2Dos16Mode>0</UserProg2Dos16Mode>
            <nStopU1X>0</nStopU1X>
            <nStopU2X>0</nStopU2X>
          </BeforeCompile>
          <BeforeMake>
            <RunUserProg1>0</RunUserProg1>
            <RunUserProg2>0</RunUserProg2>
            <UserProg1Name></UserProg1Name>
            <UserProg2Name></UserProg2Name>
            <UserProg1Dos16Mode>0</UserProg1Dos16Mode>
            <UserProg2Dos16Mode>0</UserProg2Dos16Mode>
            <nStopB1X>0</nStopB1X>
            <nStopB2X>0</nStopB2X>
          </BeforeMake>
          <AfterMake>
            <RunUserProg1>0</RunUserProg1>
            <RunUserProg2>0</RunUserProg2>
            <UserProg1Name></UserProg1Name>
            <UserProg2Name></UserProg2Name>
            <UserProg1Dos16Mode>0</UserProg1Dos16Mode>
            <UserProg2Dos16Mode>0</UserProg2Dos16Mode>
            <nStopA1X>0</nStopA1X>
            <nStopA2X>0</nStopA2X>
          </AfterMake>
          <SelectedForBatchBuild>0</SelectedForBatchBuild>
          <SVCSIdString></SVCSIdString>
        </TargetCommonOption>
        <CommonProperty>
          <UseCPPCompiler>0</UseCPPCompiler>
          <RVCTCodeConst>0</RVCTCodeConst>
          <RVCTZI>0</RVCTZI>
          <RVCTOtherData>0</RVCTOtherData>
          <ModuleSelection>0</ModuleSelection>
          <IncludeInBuild>1</IncludeInBuild>
          <AlwaysBuild>0</AlwaysBuild>
          <GenerateAssemblyFile>0</GenerateAssemblyFile>
          <AssembleAssemblyFile>0</AssembleAssemblyFile>
          <PublicsOnly>0</PublicsOnly>
          <StopOnExitCode>3</StopOnExitCode>
          <CustomArgument></CustomArgument>
          <IncludeLibraryModules></IncludeLibraryModules>
          <ComprImg>1</ComprImg>
        </CommonProperty>
        <DllOption>
          <SimDllName>SARMCM3.DLL</SimDllName>
          <SimDllArguments> -REMAP</SimDllArguments>
          <SimDlgDll>DCM.DLL</SimDlgDll>
          <SimDlgDllArguments>-pCM4</SimDlgDllArguments>
          <TargetDllName>SARMCM3.DLL</TargetDllName>
          <TargetDllArguments></TargetDllArguments>
          <TargetDlgDll>TCM.DLL</TargetDlgDll>
          <TargetDlgDllArguments>-pCM4</TargetDlgDllArguments>
        </DllOption>
        <DebugOption>
          <OPTHX>
            <HexSelection>1</HexSelection>
            <HexRangeLowAddress>0</HexRangeLowAddress>
            <HexRangeHighAddress>0</HexRangeHighAddress>
            <HexOffset>0</HexOffset>
            <Oh166RecLen>16</Oh166RecLen>
          </OPTHX>
        </DebugOption>
        <Utilities>
          <Flash1>
            <UseTargetDll>1</UseTargetDll>
            <UseExternalTool>0</UseExternalTool>
            <RunIndependent>0</RunIndependent>
            <UpdateFlashBeforeDebugging>1</UpdateFlashBeforeDebugging>
            <Capability>1</Capability>
            <DriverSelection>4096</DriverSelection>
          </Flash1>
          <bUseTDR>1</bUseTDR>
          <Flash2>BIN\\UL2CM3.DLL</Flash2>
          <Flash3>"" ()</Flash3>
          <Flash4></Flash4>
          <pFcarmOut></pFcarmOut>
          <pFcarmGrp></pFcarmGrp>
          <pFcArmRoot></pFcArmRoot>
          <FcArmLst>0</FcArmLst>
        </Utilities>
        <TargetArmAds>
          <ArmAdsMisc>
            <GenerateListings>0</GenerateListings>
            <asHll>1</asHll>
            <asAsm>1</asAsm>
            <asMacX>1</asMacX>
            <asSyms>1</asSyms>
            <asFals>1</asFals>
            <asDbgD>1</asDbgD>
            <asForm>1</asForm>
            <ldLst>0</ldLst>
            <ldmm>1</ldmm>
            <ldXref>1</ldXref>
            <BigEnd>0</BigEnd>
            <AdsALst>1</AdsALst>
            <AdsACrf>1</AdsACrf>
            <AdsANop>0</AdsANop>
            <AdsANot>0</AdsANot>
            <AdsLLst>1</AdsLLst>
            <AdsLmap>1</AdsLmap>
            <AdsLcgr>1</AdsLcgr>
            <AdsLsym>1</AdsLsym>
            <AdsLszi>1</AdsLszi>
            <AdsLtoi>1</AdsLtoi>
            <AdsLsun>1</AdsLsun>
            <AdsLven>1</AdsLven>
            <AdsLsxf>1</AdsLsxf>
            <RvctClst>0</RvctClst>
            <GenPPlst>0</GenPPlst>
            <AdsCpuType>"Cortex-M4"</AdsCpuType>
            <RvctDeviceName></RvctDeviceName>
            <mOS>0</mOS>
            <uocRom>0</uocRom>
            <uocRam>0</uocRam>
            <hadIROM>1</hadIROM>
            <hadIRAM>1</hadIRAM>
            <hadXRAM>0</hadXRAM>
            <uocXRam>0</uocXRam>
            <RvdsVP>2</RvdsVP>
            <RvdsMve>0</RvdsMve>
            <RvdsCdeCp>0</RvdsCdeCp>
            <nBranchProt>0</nBranchProt>
            <hadIRAM2>1</hadIRAM2>
            <hadIROM2>0</hadIROM2>
            <StupSel>8</StupSel>
            <useUlib>0</useUlib>
            <EndSel>0</EndSel>
            <uLtcg>0</uLtcg>
            <nSecure>0</nSecure>
            <RoSelD>3</RoSelD>
            <RwSelD>3</RwSelD>
            <CodeSel>0</CodeSel>
            <OptFeed>0</OptFeed>
            <NoZi1>0</NoZi1>
            <NoZi2>0</NoZi2>
            <NoZi3>0</NoZi3>
            <NoZi4>0</NoZi4>
            <NoZi5>0</NoZi5>
            <Ro1Chk>0</Ro1Chk>
            <Ro2Chk>0</Ro2Chk>
            <Ro3Chk>0</Ro3Chk>
            <Ir1Chk>1</Ir1Chk>
            <Ir2Chk>0</Ir2Chk>
            <Ra1Chk>0</Ra1Chk>
            <Ra2Chk>0</Ra2Chk>
            <Ra3Chk>0</Ra3Chk>
            <Im1Chk>1</Im1Chk>
            <Im2Chk>0</Im2Chk>
            <OnChipMemories>
              <Ocm1>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm1>
              <Ocm2>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm2>
              <Ocm3>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm3>
              <Ocm4>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm4>
              <Ocm5>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm5>
              <Ocm6>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </Ocm6>
              <IRAM>
                <Type>0</Type>
                <StartAddress>0x20000000</StartAddress>
                <Size>0x20000</Size>
              </IRAM>
              <IROM>
                <Type>1</Type>
                <StartAddress>0x8000000</StartAddress>
                <Size>0x80000</Size>
              </IROM>
              <XRAM>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </XRAM>
              <OCR_RVCT1>
                <Type>1</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT1>
              <OCR_RVCT2>
                <Type>1</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT2>
              <OCR_RVCT3>
                <Type>1</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT3>
              <OCR_RVCT4>
                <Type>1</Type>
                <StartAddress>0x8000000</StartAddress>
                <Size>0x80000</Size>
              </OCR_RVCT4>
              <OCR_RVCT5>
                <Type>1</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT5>
              <OCR_RVCT6>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT6>
              <OCR_RVCT7>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT7>
              <OCR_RVCT8>
                <Type>0</Type>
                <StartAddress>0x0</StartAddress>
                <Size>0x0</Size>
              </OCR_RVCT8>
              <OCR_RVCT9>
                <Type>0</Type>
                <StartAddress>0x20000000</StartAddress>
                <Size>0x20000</Size>
              </OCR_RVCT9>
              <OCR_RVCT10>
                <Type>0</Type>
                <StartAddress>0x10000000</StartAddress>
                <Size>0x10000</Size>
              </OCR_RVCT10>
            </OnChipMemories>
            <RvctStartVector></RvctStartVector>
          </ArmAdsMisc>
          <Cads>
            <interw>1</interw>
            <Optim>3</Optim>
            <oTime>0</oTime>
            <SplitLS>0</SplitLS>
            <OneElfS>1</OneElfS>
            <Strict>0</Strict>
            <EnumInt>0</EnumInt>
            <PlainCh>0</PlainCh>
            <Ropi>0</Ropi>
            <Rwpi>0</Rwpi>
            <wLevel>1</wLevel>
            <uThumb>0</uThumb>
            <uSurpInc>0</uSurpInc>
            <uC99>1</uC99>
            <uGnu>0</uGnu>
            <useXO>0</useXO>
            <v6Lang>5</v6Lang>
            <v6LangP>5</v6LangP>
            <vShortEn>1</vShortEn>
            <vShortWch>1</vShortWch>
            <v6Lto>0</v6Lto>
            <v6WtE>0</v6WtE>
            <v6Rtti>0</v6Rtti>
            <VariousControls>
              <MiscControls></MiscControls>
              <Define>{defines}</Define>
              <Undefine></Undefine>
              <IncludePath>{includes}</IncludePath>
            </VariousControls>
          </Cads>
          <Aads>
            <interw>1</interw>
            <Ropi>0</Ropi>
            <Rwpi>0</Rwpi>
            <thumb>0</thumb>
            <SplitLS>0</SplitLS>
            <SwStkChk>0</SwStkChk>
            <NoWarn>0</NoWarn>
            <uSurpInc>0</uSurpInc>
            <useXO>0</useXO>
            <ClangAsOpt>4</ClangAsOpt>
            <VariousControls>
              <MiscControls></MiscControls>
              <Define></Define>
              <Undefine></Undefine>
              <IncludePath></IncludePath>
            </VariousControls>
          </Aads>
          <LDads>
            <umfTarg>1</umfTarg>
            <Ropi>0</Ropi>
            <Rwpi>0</Rwpi>
            <noStLib>0</noStLib>
            <RepFail>1</RepFail>
            <useFile>0</useFile>
            <TextAddressRange>0x08000000</TextAddressRange>
            <DataAddressRange>0x20000000</DataAddressRange>
            <pXoBase></pXoBase>
            <ScatterFile></ScatterFile>
            <IncludeLibs></IncludeLibs>
            <IncludeLibsPath></IncludeLibsPath>
            <Misc></Misc>
            <LinkerInputFile></LinkerInputFile>
            <DisabledWarnings></DisabledWarnings>
          </LDads>
        </TargetArmAds>
      </TargetOption>
      <Groups>
{groups}      </Groups>
    </Target>
  </Targets>
</Project>
"""


def main() -> int:
    MDK_DIR.mkdir(parents=True, exist_ok=True)

    groups = collect()
    total = sum(len(files) for _, files in groups)
    if total == 0:
        print("[ERROR] 没有找到任何源文件，请检查目录结构。")
        return 1

    group_text = "".join(group_xml(name, files) for name, files in groups)

    xml = TEMPLATE.format(
        target_name=TARGET_NAME,
        device=DEVICE,
        vendor=VENDOR,
        pack_id=PACK_ID,
        cpu=CPU_STRING,
        defines=DEFINES,
        includes=";".join(INCLUDE_DIRS),
        groups=group_text,
    )

    OUT_FILE.write_text(xml, encoding="utf-8", newline="\r\n")
    print(f"[OK] 已生成 {OUT_FILE}")
    for name, files in groups:
        print(f"     - {name:<14} {len(files):>4} 个文件")
    print(f"     合计 {total} 个源文件")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
