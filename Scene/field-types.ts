import type { ArchiveCell } from './archive-loop';

export type FieldParameters = {
    originRow: number;
    originLane: number;
    scanTime: number;
    scanBlend: number;
    shoulder: number;
    laneFocus: number;
    idleGain: number;
    time: number;
    flatMix: number;
    pulseGain: number;
    selectionWave: boolean;
    pulses: { row: number; lane: number; time: number }[];
};
export type FieldGridParameters = FieldParameters & {
    row0: number;
    lane0: number;
    rows: number;
    lanes: number;
};
export type FieldSampler = (row: number, lane: number) => number | undefined;
export type NativeField = (parameters: FieldParameters, cells: ArchiveCell[]) => FieldSampler | undefined;
