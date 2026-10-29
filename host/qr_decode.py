"""从图片中解出二维码文本，供上位机「导入图片」使用。

固件只能按文本现场生成二维码，不能接收图片；因此「把一张带二维码的图片
显示到屏幕上」的正确做法是：在 PC 侧把图片里的码解成文本，再把文本发给
设备重新编码。本模块负责其中的解码一步。

依赖（都只在调用时才导入，未安装不影响上位机其它功能）：
- ``Pillow``：读取 PNG/JPG/BMP 等常见格式；
- ``zxing-cpp``：二维码识别。比 OpenCV 轻得多（约 1 MB），且支持
  旋转、缩放、反色三种常见变形。

典型用法::

    from qr_decode import decode_qr_image, QrDecodeError
    try:
        result = decode_qr_image("wechat_card.jpg")
    except QrDecodeError as exc:
        print("解码失败：", exc)
    else:
        print(result.text, result.byte_length)
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

__all__ = [
    "SUPPORTED_SUFFIXES",
    "QrDecodeError",
    "DecodedQr",
    "decoder_available",
    "install_hint",
    "decode_qr_image",
]

#: 文件对话框里列出的图片后缀（Pillow 实际能读的远不止这些）。
SUPPORTED_SUFFIXES = (".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".tif", ".tiff")

#: 未安装依赖时给出的安装命令。
_INSTALL_COMMAND = "python -m pip install Pillow zxing-cpp"


class QrDecodeError(RuntimeError):
    """图片解码失败（依赖缺失、文件不可读、图中无码等），消息可直接展示。"""


@dataclass(frozen=True)
class DecodedQr:
    """一次成功解码的结果。"""

    text: str
    """解码得到的原文（未做任何转义或截断）。"""

    format: str
    """码制名称，例如 ``QR Code``。"""

    count: int
    """图片中被识别出的码总数（只取第一个，其余仅用于提示）。"""

    source: str
    """图片路径，便于日志记录。"""

    @property
    def byte_length(self) -> int:
        """文本的 UTF-8 字节数（固件上限 256）。"""
        return len(self.text.encode("utf-8"))


def install_hint() -> str:
    """返回依赖缺失时的安装命令。"""
    return _INSTALL_COMMAND


def decoder_available() -> bool:
    """判断 Pillow 与 zxing-cpp 是否都已安装（不抛异常）。"""
    try:
        _import_backends()
    except QrDecodeError:
        return False
    return True


def _import_backends():
    """延迟导入两个可选依赖，缺失时抛出带安装提示的 :class:`QrDecodeError`。"""
    try:
        from PIL import Image  # 延迟导入：未装 Pillow 时不影响界面启动
    except ImportError as exc:  # pragma: no cover - 取决于环境
        raise QrDecodeError(
            f"缺少 Pillow，无法读取图片。请先安装：{_INSTALL_COMMAND}"
        ) from exc
    try:
        import zxingcpp  # 延迟导入：未装 zxing-cpp 时不影响界面启动
    except ImportError as exc:  # pragma: no cover - 取决于环境
        raise QrDecodeError(
            f"缺少 zxing-cpp，无法识别二维码。请先安装：{_INSTALL_COMMAND}"
        ) from exc
    return Image, zxingcpp


def _format_name(code_format: object) -> str:
    """把 zxing-cpp 的 BarcodeFormat 枚举转成人类可读的名字。"""
    name = str(getattr(code_format, "name", code_format))
    if name.startswith("BarcodeFormat."):
        name = name.split(".", 1)[1]
    return {
        "QRCode": "QR Code",
        "MicroQRCode": "Micro QR Code",
        "DataMatrix": "Data Matrix",
        "Aztec": "Aztec",
        "PDF417": "PDF417",
    }.get(name, name)


def decode_qr_image(path: str | Path, *, try_harder: bool = True) -> DecodedQr:
    """解码图片中的二维码。

    参数:
        path: 图片路径。
        try_harder: 首次未识别时，再试一次灰度 + 2 倍放大。
            手机截图里的码通常一次就能识别，拍照（有反光/透视）时该兜底有用。

    返回:
        :class:`DecodedQr`（多个码时取第一个，``count`` 记录总数）。

    异常:
        :class:`QrDecodeError`: 依赖缺失、文件不存在、格式不支持或图中没有二维码。
    """
    Image, zxingcpp = _import_backends()

    image_path = Path(path)
    if not image_path.is_file():
        raise QrDecodeError(f"图片不存在：{image_path}")

    try:
        with Image.open(image_path) as raw:
            image = raw.convert("RGB")
    except Exception as exc:  # Pillow 会抛多种异常，统一转换为可展示错误
        raise QrDecodeError(f"无法读取图片 {image_path.name}：{exc}") from exc

    results = _read_codes(zxingcpp, image)

    if not results and try_harder:
        # 兜底：灰度 + 放大 2 倍（最近邻，不做插值，避免破坏模块边界）。
        try:
            bigger = image.convert("L").resize(
                (image.width * 2, image.height * 2), Image.NEAREST
            )
            results = _read_codes(zxingcpp, bigger)
        except Exception:  # pragma: no cover - 极端图片尺寸才会触发
            results = []

    if not results:
        raise QrDecodeError(
            f"图片 {image_path.name} 中没有识别到二维码。\n"
            "请确认图片清晰、码完整、没有被裁切或大面积遮挡。"
        )

    first = results[0]
    text = first.text or ""
    if not text:
        raise QrDecodeError("识别到二维码，但没有解析出文本内容。")

    return DecodedQr(
        text=text,
        format=_format_name(first.format),
        count=len(results),
        source=str(image_path),
    )


def _read_codes(zxingcpp, image):
    """调用 zxing-cpp 识别，返回结果列表（失败时返回空列表）。"""
    try:
        results = zxingcpp.read_barcodes(
            image,
            try_rotate=True,
            try_downscale=True,
            try_invert=True,
        )
    except TypeError:
        # 老版本 zxing-cpp 没有 try_invert 参数。
        results = zxingcpp.read_barcodes(
            image, try_rotate=True, try_downscale=True
        )
    except Exception as exc:  # pragma: no cover - 原生库异常
        raise QrDecodeError(f"二维码识别失败：{exc}") from exc
    return list(results or [])
