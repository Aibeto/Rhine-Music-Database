import { archiveColumns, columnFiles, fileLocation } from './data.ts';
export type ArchiveCell = {
    lane: number;
    row: number;
};
export type ArchiveNavigation =
    | {
          axis: 'row' | 'lane';
          direction: number;
      }
    | {
          cell: ArchiveCell;
      };
export const LOOP_COLUMNS = 9;
export const LOOP_ROWS = 32;
export const COLUMN_SPACING = 5.2;
export const ROW_SPACING = 0.62;
const POOL_LANES = [0, 1, 2, 3, 4, -2, -1, 5, 6];
export function wrap(value: number, count: number) {
    return ((value % count) + count) % count;
}

// 在无限序列中选择某条目的一个出现位置；定向移动使用相邻格，
// 因此从最后一项回到第一项时不会反向。
export function nearestOccurrence(value: number, center: number, period: number) {
    return value + Math.floor((center - value + period / 2) / period) * period;
}

export function fileAtCell({ lane, row }: ArchiveCell) {
    const files = columnFiles(wrap(lane, archiveColumns.length));
    return files[wrap(row - 12, files.length)];
}

export function selectionCell(
    index: number,
    current: ArchiveCell,
    navigation?: ArchiveNavigation,
): ArchiveCell {
    if (navigation && 'cell' in navigation) {
        return {
            ...navigation.cell,
        };
    }
    const next = fileLocation(index);
    const row = nearestOccurrence(next.row, current.row, columnFiles(next.lane).length);
    if (navigation?.axis === 'row') {
        return {
            lane: current.lane,
            row: current.row + navigation.direction,
        };
    }
    return {
        lane:
            navigation?.axis === 'lane'
                ? current.lane + navigation.direction
                : nearestOccurrence(next.lane, current.lane, archiveColumns.length),
        row,
    };
}

// 保留参考动画最初的 160 个实例；交互时另外四列
// 作为两侧隐藏余量。
export function poolCell(index: number): ArchiveCell {
    return {
        lane: POOL_LANES[Math.floor(index / LOOP_ROWS)],
        row: index % LOOP_ROWS,
    };
}

export function visibleCell(index: number, center: ArchiveCell): ArchiveCell {
    return {
        lane: nearestOccurrence(POOL_LANES[Math.floor(index / LOOP_ROWS)], center.lane, LOOP_COLUMNS),
        row: nearestOccurrence(index % LOOP_ROWS, center.row, LOOP_ROWS),
    };
}

export function cellKey(cell: ArchiveCell) {
    return `${cell.lane}:${cell.row}`;
}

export function sameCell(a: ArchiveCell, b: ArchiveCell) {
    return a.lane === b.lane && a.row === b.row;
}
