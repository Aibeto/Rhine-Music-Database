import type { NativeField, FieldGridParameters } from '../Scene/field-types';
import { archiveWave, idleWave, settlingWave, columnStrength, baselineSelectionWave } from '../Scene/motion';

/**
 * 把可见格、当前卡片和归位卡片包围成有限网格，一次跨引擎调用计算基础位移。
 * 极端跨度或新相机边界落在网格外时返回 undefined，由原公式按需补齐。
 */
export function createNativeField(provider: (parameters: FieldGridParameters) => ArrayBuffer): NativeField {
    return (parameters, cells) => {
        if (!cells.length) {
            return undefined;
        }
        let minRow = Infinity;
        let maxRow = -Infinity;
        let minLane = Infinity;
        let maxLane = -Infinity;
        for (const cell of cells) {
            minRow = Math.min(minRow, cell.row);
            maxRow = Math.max(maxRow, cell.row);
            minLane = Math.min(minLane, cell.lane);
            maxLane = Math.max(maxLane, cell.lane);
        }
        const row0 = Math.floor(minRow) - 32;
        const lane0 = Math.floor(minLane) - 3;
        const rows = (Math.ceil(maxRow) + 32 - row0) * 2 + 1;
        const lanes = Math.ceil(maxLane) + 3 - lane0 + 1;
        if (rows * lanes > 32768) {
            return undefined;
        }
        const values = new Float64Array(provider({ ...parameters, row0, lane0, rows, lanes }));
        if (values.length !== rows * lanes) {
            return undefined;
        }
        return (row, lane) => {
            const rowIndex = (row - row0) * 2;
            const laneIndex = lane - lane0;
            if (
                !Number.isInteger(rowIndex) ||
                !Number.isInteger(laneIndex) ||
                rowIndex < 0 ||
                rowIndex >= rows ||
                laneIndex < 0 ||
                laneIndex >= lanes
            ) {
                return undefined;
            }
            return values[laneIndex * rows + rowIndex];
        };
    };
}

/** 直接以原始运动函数校验原生网格，覆盖扫描、坐标重定位、待机和脉冲边界。 */
export function testNativeField(provider: (parameters: FieldGridParameters) => ArrayBuffer) {
    for (let variant = 0; variant < 8; variant++) {
        const time = 24 + variant * 0.61;
        const parameters: FieldGridParameters = {
            row0: -16,
            lane0: -3,
            rows: 81,
            lanes: 9,
            originRow: variant % 2 ? 512 : 0,
            originLane: variant % 2 ? -5 : 0,
            scanTime: 22 + variant * 0.61,
            scanBlend: variant % 3 ? 0.7 : 0,
            shoulder: variant * 0.71 - 3,
            laneFocus: variant * 0.21,
            idleGain: variant / 7,
            time,
            flatMix: variant / 7,
            pulseGain: 1 - variant / 14,
            selectionWave: variant % 2 === 0,
            pulses: [-0.1, 0, 0.2, 1.4, 3.2, 3.3].map((age, index) => ({
                row: index - 2,
                lane: index % 5,
                time: time - age,
            })),
        };
        const values = new Float64Array(provider(parameters));
        if (values.length !== parameters.rows * parameters.lanes) {
            return false;
        }
        for (let laneIndex = 0; laneIndex < parameters.lanes; laneIndex++) {
            const lane = parameters.lane0 + laneIndex;
            for (let rowIndex = 0; rowIndex < parameters.rows; rowIndex++) {
                const row = parameters.row0 + rowIndex * 0.5;
                let ripple = 0;
                if (parameters.selectionWave) {
                    for (const pulse of parameters.pulses) {
                        ripple += baselineSelectionWave(
                            Math.hypot(row - pulse.row, (lane - pulse.lane) * 2.2),
                            time - pulse.time,
                        );
                    }
                }
                const expected =
                    (archiveWave(
                        row + parameters.originRow,
                        lane + parameters.originLane,
                        parameters.scanTime,
                    ) *
                        parameters.scanBlend +
                        settlingWave(row - parameters.shoulder, 26.56) *
                            columnStrength(lane, parameters.laneFocus)) *
                        (1 - parameters.flatMix) +
                    idleWave(row + parameters.originRow, lane + parameters.originLane, time) *
                        parameters.idleGain +
                    Math.max(-0.6, Math.min(0.6, ripple)) * parameters.pulseGain;
                const actual = values[laneIndex * parameters.rows + rowIndex];
                if (Math.abs(actual - expected) > 1e-12 * Math.max(1, Math.abs(expected))) {
                    return false;
                }
            }
        }
    }
    return true;
}
