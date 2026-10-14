import { MusicStore, item, parse } from './music.js';
import { HUD } from './hud.js';
import { ReelCurve, SurfaceMotion } from './motion.js';
/**
 * 建立音乐状态、HUD 和输入转发入口。
 * Host 提供平台能力，runtime 提供场景；状态变化统一通知 Host 重绘。
 */
export function create(host, glyphs, runtime, content) {
    const music = new MusicStore(host, runtime, content);
    const hud = new HUD(host, glyphs, music, runtime);
    let dragging = false;
    let down = [0, 0];
    let moved = false;
    music.onChange = () => host.invalidate?.();
    const app = {
        music,
        hud,
        runtime,
        parse,
        item,
        ReelCurve,
        SurfaceMotion,
        /**
         * 组装本帧场景与 HUD 数据，交给原生渲染层。
         * 只在需要时读取场景缓冲区；音乐观察、HUD 绘制和状态交付保持原顺序。
         */
        step(t, w, reuseScene = false) {
            const frame = reuseScene ? music.frame : runtime.frameFast(t);
            if (!frame) {
                return null;
            }
            for (const draw of frame.draws || []) {
                draw.matrices =
                    draw.matrixBuffer < 0
                        ? []
                        : Array.from(runtime.buffer(draw.matrixBuffer)).slice(0, draw.count * 16);
                draw.themes =
                    draw.themeBuffer < 0
                        ? []
                        : Array.from(runtime.buffer(draw.themeBuffer)).slice(0, draw.count);
            }
            music.frame = frame;
            music.observe(frame.selected);
            if (hud.tab !== 'overview' && music.wantsSongDetails && frame.mode === 'detail') {
                music.sameAlbum();
            }
            const drawing = hud.draw(frame, w, t);
            const bounds = hud.modalBounds(w);
            const state = {
                inputX: bounds.x + 53,
                inputY: bounds.y + 180,
                inputWidth: bounds.width - 106,
                seek: hud.playerSeekRect,
                volume: hud.playerVolumeRect,
                loginMethod: music.loginMethod,
                formKind: hud.formKind || '',
                formPID: hud.formPID || '',
                modal: hud.modal || '',
                query: hud.query,
                closing: hud.closing,
                modalOpacity: hud.motion.opacity,
                modalOffset: hud.motion.offset,
                dark: hud.dark,
                reduced: hud.reduced,
                mode: frame.mode,
                viewer: frame.viewer,
                status: music.status,
            };
            return {
                commands: drawing.commands,
                hits: drawing.hits,
                state,
                current: music.current || {},
                cover: music.coverImage,
            };
        },
        poll(t, force = false) {
            music.player.tick(t, force);
        },
        mediaAction(value) {
            if (value === 'resume' || value === 'pause') {
                host.mediaControl(value);
            } else {
                hud.action('player:' + value);
            }
        },
        /**
         * 校验当前弹窗提交的字段，再调用登录或歌单操作。
         * 密码只用于本次请求，歌单成功后才关闭弹窗。
         */
        form(raw) {
            let data;
            try {
                data = JSON.parse(raw);
            } catch (error) {
                return;
            }
            if (
                hud.modal === 'login' &&
                ['phone', 'email'].includes(music.loginMethod) &&
                typeof data.account === 'string' &&
                typeof data.password === 'string' &&
                data.account.length <= 100 &&
                data.password.length <= 256
            ) {
                music.passwordLogin(music.loginMethod, data.account, data.password);
            }
            if (hud.modal === 'playlist-form' && typeof data.name === 'string') {
                Promise.resolve(music.playlistForm(hud.formKind, data.name, hud.formPID)).then((ok) => {
                    if (ok) {
                        hud.closeModal();
                    }
                });
            }
        },
        action(value) {
            hud.action(value);
        },
        query(value) {
            hud.setQuery(value);
        },
        key(key, command) {
            hud.key(key, command);
        },
        wheel(x, y, delta, steps, distance) {
            hud.wheel(x, y, delta, steps, distance);
        },
        pointer(type, x, y) {
            if (hud.modal) {
                return;
            }
            if (type === 'pointerdown') {
                dragging = true;
                down = [x, y];
                moved = false;
            }
            if (type === 'pointermove' && dragging && Math.hypot(x - down[0], y - down[1]) > 6) {
                moved = true;
            }
            if (type === 'pointerup' && dragging) {
                const picked = runtime.pick(x, y);
                runtime.pointer(type, x, y);
                dragging = false;
                if (!moved && hud.frame?.mode === 'archive' && picked !== null) {
                    music.observe(runtime.selection());
                    if (music.current && music.current.kind !== 'song') {
                        music.enterDirectory(music.current);
                    }
                }
                return;
            }
            runtime.pointer(type, x, y);
        },
        animating(t) {
            return (
                !hud.motion.finished ||
                hud.lyricsAnimating(t) ||
                hud.closing ||
                t - (hud.resultStart ?? -100) < 0.5 ||
                Object.values(hud.rolling).some((label) => label.active?.(t))
            );
        },
        state: () => ({
            uid: music.uid,
            current: music.current,
            lists: music.lists,
            directory: music.directory,
            status: music.status,
            totals: music.totals,
            offsets: music.offsets,
        }),
        async init() {
            await runtime.init();
            runtime.theme(hud.dark);
            runtime.reduced(hud.reduced);
            const volume = Number(host.preference?.('volume') || 0.7);
            if (host.mediaControl && Number.isFinite(volume)) {
                music.player.volume(volume);
            }
            music.restoreAccount();
            music.restoreIdentityName();
            return true;
        },
    };
    return app;
}
globalThis.RhineQt = {
    create,
};
