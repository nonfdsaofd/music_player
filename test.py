import os
import subprocess
import json
from pathlib import Path
import shutil

# ====================== 配置区（请修改这里的路径）======================
SOURCE_FOLDER = r"D:\Music"  # Windows示例
OUTPUT_FOLDER = r"D:\coding\test_nana\build\music"   # Windows示例
OVERWRITE = False  # 是否覆盖已存在的MP3文件
# 通用兼容参数（全版本FFmpeg通用）
MP3_BITRATE = "192k"
MP3_SAMPLERATE = "44100"
MP3_CHANNELS = "2"
MP3_QUALITY = "4"
MIN_AUDIO_DURATION = 1.0
# =====================================================================

def check_dependencies() -> bool:
    """检查ffmpeg/ffprobe是否安装，且磁盘有写入权限"""
    try:
        subprocess.run(['ffmpeg', '-version'], capture_output=True, check=True)
        subprocess.run(['ffprobe', '-version'], capture_output=True, check=True)
        test_file = Path(OUTPUT_FOLDER) / "test_permission.tmp"
        test_file.touch(exist_ok=True)
        test_file.unlink()
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("错误：未检测到ffmpeg/ffprobe，请先安装并添加到系统PATH")
        return False
    except PermissionError:
        print(f"错误：输出文件夹无写入权限 → {OUTPUT_FOLDER}")
        return False

def get_audio_stream_info(input_file: Path) -> dict:
    """解析源文件音频流，兼容stream/format层级的时长"""
    cmd = [
        'ffprobe',
        '-v', 'error',
        '-select_streams', 'a',
        '-show_entries', 'stream=codec_name,bit_rate,sample_rate,channels,duration',
        '-show_entries', 'format=duration',  # 新增：提取format层级时长
        '-of', 'json',
        str(input_file)
    ]
    try:
        result = subprocess.run(cmd, capture_output=True, check=True, text=True, timeout=10)
        data = json.loads(result.stdout)
        streams = data.get('streams', [])
        if not streams:
            return {'valid': False, 'reason': '无音频流'}
        
        # 优先读取stream层级时长，无则读取format层级
        stream = streams[0]
        stream_duration = stream.get('duration', 0)
        format_duration = data.get('format', {}).get('duration', 0)
        duration = float(stream_duration) if stream_duration else float(format_duration)
        
        # 读取比特率（兼容无bit_rate的情况）
        bit_rate = stream.get('bit_rate', 0)
        
        # 过滤异常：时长<1秒视为无效
        if duration < 1:
            return {'valid': False, 'reason': f'音频时长异常（{duration}秒）'}
        
        return {
            'valid': True,
            'is_mp3': stream.get('codec_name') == 'mp3',
            'stream_count': len(streams)
        }
    except Exception as e:
        return {'valid': False, 'reason': f'解析失败：{str(e)[:50]}'}

def validate_mp3_file(mp3_file: Path) -> bool:
    """校验转换后的MP3是否完整可播放"""
    if not mp3_file.exists() or mp3_file.stat().st_size < 1024:
        return False
    # 仅捕获致命错误，屏蔽警告
    cmd = ['ffmpeg', '-v', 'panic', '-i', str(mp3_file), '-f', 'null', '-']
    result = subprocess.run(cmd, capture_output=True)
    return result.returncode == 0

def convert_single_file(input_file: Path, output_file: Path, overwrite: bool) -> bool:
    """核心转换逻辑（仅保留全版本通用参数）"""
    if output_file.exists() and not overwrite:
        print(f"[跳过] {input_file.name} → 已存在（不覆盖）")
        return False

    audio_info = get_audio_stream_info(input_file)
    if not audio_info['valid']:
        print(f"[跳过] {input_file.name} → {audio_info['reason']}")
        return False

    # 最终版通用FFmpeg命令（移除所有小众参数）
    base_cmd = [
        'ffmpeg', 
        '-hide_banner', 
        '-loglevel', 'warning',  # 显示必要警告，便于调试
        '-i', str(input_file),
        '-vn',                        # 禁用视频流
        '-map', '0:a:0',              # 仅选第一个音频流
        '-c:a', 'libmp3lame',         # MP3编码器（全版本通用）
        '-b:a', MP3_BITRATE,          # 固定比特率（兼容所有播放器）
        '-ar', MP3_SAMPLERATE,        # 标准采样率
        '-ac', MP3_CHANNELS,          # 标准双声道
        '-q:a', MP3_QUALITY,          # LAME质量预设（替代lameopts）
        '-id3v2_version', '3',        # ID3v2标签（通用）
        '-write_xing', '1',           # 若仍报错可删除此行（影响仅为时长估算）
        '-threads', '0',              # 多核加速（通用）
        '-y' if overwrite else '-n',  # 覆盖/不覆盖开关
        str(output_file)
    ]

    # 打印调试命令（方便手工执行）
    print(f"\n[调试] 执行命令：{' '.join(base_cmd)}")
    
    try:
        result = subprocess.run(
            base_cmd, 
            check=True, 
            timeout=600,
            capture_output=True,
            text=True
        )
        
        if validate_mp3_file(output_file):
            print(f"[成功] {input_file.name} → {output_file.name}（兼容模式）")
            return True
        else:
            if output_file.exists():
                output_file.unlink()
            print(f"[失败] {input_file.name} → 转换后文件损坏")
            return False
    except subprocess.TimeoutExpired:
        if output_file.exists():
            output_file.unlink()
        print(f"[失败] {input_file.name} → 转换超时")
        return False
    except subprocess.CalledProcessError as e:
        if output_file.exists():
            output_file.unlink()
        print(f"\n[错误] {input_file.name} → FFmpeg执行失败")
        print(f"返回码：{e.returncode}")
        print(f"FFmpeg错误输出：{e.stderr}")
        return False
    except Exception as e:
        if output_file.exists():
            output_file.unlink()
        print(f"\n[错误] {input_file.name} → 未知错误：{str(e)}")
        return False

def batch_convert() -> None:
    """批量转换主逻辑"""
    source_dir = Path(SOURCE_FOLDER).resolve()
    output_dir = Path(OUTPUT_FOLDER).resolve()

    if not source_dir.is_dir():
        print(f"错误：源文件夹不存在 → {source_dir}")
        return

    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"开始高兼容转换 → 源：{source_dir} | 输出：{output_dir}\n")

    stats = {'total': 0, 'success': 0, 'failed': 0, 'skipped': 0}

    # 递归遍历所有文件，排除隐藏文件
    for file in source_dir.rglob("*"):
        if file.is_file() and not file.name.startswith('.'):
            stats['total'] += 1
            output_file = output_dir / file.with_suffix('.mp3').name
            success = convert_single_file(file, output_file, OVERWRITE)
            if success:
                stats['success'] += 1
            elif output_file.exists() and not OVERWRITE:
                stats['skipped'] += 1
            else:
                stats['failed'] += 1

    # 最终统计
    print("\n=== 转换完成 ===")
    print(f"总文件数：{stats['total']}")
    print(f"成功转换（可播放）：{stats['success']}")
    print(f"转换失败（损坏/异常）：{stats['failed']}")
    print(f"跳过文件：{stats['skipped']}")
    print(f"\n输出文件夹：{output_dir}")

def main():
    if not check_dependencies():
        return
    batch_convert()

if __name__ == '__main__':
    main()