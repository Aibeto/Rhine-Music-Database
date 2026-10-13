/** 影片期间及 1920×1080 窗口下保持精确的参考坐标。 */
export function openingLayout(width: number, height: number) {
    width = Math.max(1, width);
    height = Math.max(1, height);
    const scale = Math.min(height / 1080, width / 1280);
    return {
        width: width / scale,
        height: height / scale,
        scale,
        kind: 'opening' as const,
    };
}

export function viewportLayout(width: number, height: number, coarse: boolean, cinematic = false) {
    width = Math.max(1, width);
    height = Math.max(1, height);
    if (cinematic) {
        return {
            width: 1920,
            height: 1080,
            scale: Math.min(width / 1920, height / 1080),
            kind: 'cinematic' as const,
        };
    }
    const portrait = width / height < 1.05;
    const compact = portrait || width < 1100 || (coarse && height < 600);
    const scale = compact ? 1 : height / 1080;
    // 保持原有判断顺序，仅计算当前需要的分支结果。
    // 按窗口比例与紧凑模式确定布局类别。
    const selectLayoutKind = () => {
        if (portrait) {
            return 'portrait' as const;
        } else if (compact) {
            return 'compact' as const;
        } else {
            return 'desktop' as const;
        }
    };
    return {
        width: width / scale,
        height: height / scale,
        scale,
        kind: selectLayoutKind(),
    };
}

/** 保持长焦透视，只重新构图相机，不移动档案。 */
export function archiveFraming(
    width: number,
    height: number,
    span: number,
    detail: number,
    compact: boolean,
) {
    const aspect = width / height;
    const portrait = aspect < 1.05;
    const baseSpan = span + (5.9 - span) * detail;
    const portraitDetailSpan = Math.max(6.3 / aspect, (3.7 * height) / Math.max(100, 0.54 * height - 156));
    const viewSpan = portrait
        ? Math.max(baseSpan, 8.4 / aspect + (portraitDetailSpan - 8.4 / aspect) * detail)
        : Math.max(baseSpan, (baseSpan * (16 / 9)) / aspect);
    // 保持原有判断顺序，仅计算当前需要的分支结果。
    // 按竖屏、紧凑或桌面布局选择详情水平锚点。
    const selectDetailAnchorX = () => {
        if (portrait) {
            return 0.5;
        } else if (compact) {
            return 0.27;
        } else {
            return 550 / 1920;
        }
    };
    // 保持原有判断顺序，仅计算当前需要的分支结果。
    // 按布局选择详情垂直锚点，保留高度补偿。
    const selectDetailAnchorY = () => {
        if (portrait) {
            return 0.27 + 34 / height;
        } else if (compact) {
            return 0.49;
        } else {
            return 560 / 1080;
        }
    };
    return {
        span: viewSpan,
        portrait,
        // 竖屏的选中位置有意放在标题和导航上方。
        previewY: portrait ? 0.36 : 0.5,
        detailX: selectDetailAnchorX(),
        detailY: selectDetailAnchorY(),
    };
}
