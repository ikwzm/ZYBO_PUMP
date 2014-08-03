ZYBO_PUMP
---------

###概要###

このプロジェクトは、Digilent社(<http://www.digilentinc.com>)のFPGAボードZYBOで 
PUMP_AXI4(<http://github.com/ikwzm/PUMP_AXI4>)と
LED_AXI(<http://github.com/ikwzm/LED_AXI>)を実装し動作を確認するするためのものです.

###開発環境###

FPGAのビットイメージは以下の開発環境で合成出来ることを確認しています.

* Xilinx Vivado 2014.2
* Xilinx SDK 2014.2

###インストール###

####1. クリーンなプロジェクトからFPGAのビットイメージを生成する場合.####

#####1.1. githubからリポジトリをダウンロードする.#####

shell> git clone git://github.com/ikwzm/ZYBO_PUMP.git ZYBO_PUMP

shell> cd ZYBO_PUMP

shell> git checkout clean-project

出来れば何もないところから始めると良い。

#####1.2. githubからサブモジュール(PUMP_AXI4とLED_AXI)をダウンロードする.#####

shell> git submodule init

shell> git submodule update

#####1.3. Vivado でプロジェクトを開く.#####

Vivado> OpenProject > ZYBO_PUMP/project/project.xpr

#####1.4. 必要なソースファイルを生成する.#####

Vivado> Flow Navigator > Generate Block Design > Generate

#####1.5. 論理合成をする.#####

Vivado> Flow Navigator > Run Synthesis

#####1.6. 配置配線をする.#####

Vivado> Flow Navigator > Run Implementation 

#####1.7. ビットストリームファイルを生成する.#####

Vivado> Flow Navigator > Generate Bitstream

これで project/project.runs/impl_1/design_1_wrapper.bit が出来ます.

#####1.8. SDKのためにハードウェア情報をエクスポートする.#####

Vivado> File > Export > Export Hardware > Export to: <Local to Project> > OK

これで project/project.sdk/design_1_wrapper.hdf が出来ます.

#####1.9. SDKを起動する.#####

Vivado> File > Launch SDK > Exported location:<Local to Project> Workspace:<Local Project> > OK

#####1.10. FSBL(First Stage Boot Loader)を生成する.#####

SDK> File > New > Aplication Project

次のようなウインドウがポップするので Project name と Template を入力.

    New Project
    
      Project name: fsbl <- 入力
    
      Target Hardware     
    
        Hardware Platfrom: design_1_wrapper_hw_platform_0 <- 最初から選択されている
    
        Processor: ps7_cortexa9_0  <- 最初から選択されている
    
      Target Software
    
        Language: C <- 最初から選択されている
    
        OS Platform: standalone <- 最初から選択されている
    
        Board Support Package: Create New fsbl_bsp  <- Project Nameを入力すると自動的に入力される
    
      Template: Zynq FSBL  <- 選択(この項目は次の(NEXT>)タグを押すと出てきます)

Finishボタンを押すと自動的に project/project.sdk/fsbl/Debug/fsbl.elf が生成されます.

#####1.11. u-bootを生成する.#####

このプロジェクトでは u-boot の作成は行いません. 
とりあえず適当に作った u-boot.elf を ./boot に入れておきました.

#####1.12. BOOT.binを生成する.#####

Xilinx社のツールが起動するシェル環境(Vivado Tcl Shellなど)で次のコマンドを入力.

shell> cd boot/

shell> bootgen -image u-boot.bif -w on -o BOOT.bin

###ライセンス###

二条項BSDライセンス (2-clause BSD license) で公開しています。

