// 共享原有动画时间轴，由 Qt QJSEngine 执行，不依赖 DOM 或 WebView。
import { bootMotion } from '../Scene/boot-motion';
import * as motion from '../Scene/motion';
import { decryptionFrame, DecryptionController } from '../Scene/decryption';
import { frostedTransmissionLod, glassRevealAtHeight } from '../Scene/glass-reveal';
import { archiveFraming, viewportLayout, openingLayout } from '../Scene/viewport-layout';
Object.assign(globalThis, {
    RhineTimeline: {
        bootMotion,
        decryptionFrame,
        DecryptionController,
        frostedTransmissionLod,
        glassRevealAtHeight,
        archiveFraming,
        viewportLayout,
        openingLayout,
        ...motion,
    },
});
