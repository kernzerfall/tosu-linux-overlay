import {
    checkGameOverlayConfig,
    context,
    downloadFile,
    getProgramPath,
    platformResolver,
    unzip,
    verifyDownload,
    wLogger
} from '@tosu/common';
import {
    ChildProcess,
    type ChildProcessByStdio,
    spawn
} from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { chmod, mkdir, rm } from 'node:fs/promises';
import path from 'node:path';
import { Readable } from 'node:stream';

const platform = platformResolver(process.platform);

const isLinux = process.platform === 'linux';
const overlayExecutable = isLinux
    ? 'tosu-ingame-overlay'
    : 'tosu-ingame-overlay.exe';

function isOverlayAsset(name: string) {
    if (!name.includes('tosu-overlay') || !name.endsWith('.zip')) return false;
    return isLinux ? name.includes('linux') : !name.includes('linux');
}

/**
 * On Linux the overlay is composited by libtosu-overlay.so inside the game, which
 * must be loaded at game start. Tell the user how to launch osu! with it.
 */
function logLinuxLaunchHint(gameOverlayPath: string) {
    const runner = path.join(gameOverlayPath, 'hook', 'tosu-overlay-run');
    wLogger.warn(
        `In-game overlay on Linux needs osu! to be started through: %${runner} <osu! executable>%`
    );
    // the logger strips %...% pairs, so the Steam %command% syntax lives in the README
    wLogger.warn(
        'For Steam and other launchers see https://github.com/tosuapp/tosu/blob/master/packages/ingame-overlay/README.md#linux'
    );
}

// release zips do not keep unix permissions
const LINUX_EXECUTABLES = [
    'tosu-ingame-overlay',
    'chrome_crashpad_handler',
    'chrome-sandbox',
    path.join('hook', 'tosu-overlay-run')
];

async function restoreLinuxPermissions(gameOverlayPath: string) {
    for (const file of LINUX_EXECUTABLES) {
        const filePath = path.join(gameOverlayPath, file);
        if (existsSync(filePath)) await chmod(filePath, 0o755);
    }
}

export async function runOverlay(): Promise<ChildProcess | Error> {
    try {
        if (process.platform !== 'win32' && !isLinux) {
            throw new Error(
                'This feature is currently only available on Windows and Linux'
            );
        }

        await checkGameOverlayConfig();

        const gameOverlayPath = path.join(getProgramPath(), 'game-overlay');
        if (
            existsSync(path.join(gameOverlayPath)) &&
            !existsSync(path.join(gameOverlayPath, 'version'))
        ) {
            // old overlay detected, removing it
            wLogger.warn('Old in-game overlay version detected. Removing...');
            await rm(gameOverlayPath, { recursive: true, force: true });
        }

        if (existsSync(path.join(gameOverlayPath, 'version'))) {
            const overlayVersion = readFileSync(
                path.join(gameOverlayPath, 'version'),
                'utf8'
            );
            if (overlayVersion.trimEnd() !== context.currentVersion) {
                await rm(gameOverlayPath, { recursive: true, force: true });

                wLogger.warn('In-game overlay update available. Updating...');
            }
        }

        if (!existsSync(gameOverlayPath)) {
            const archivePath = path.join(
                gameOverlayPath,
                'tosu-gameoverlay.zip'
            );

            await mkdir(gameOverlayPath, { recursive: true });

            const request = await fetch(
                `https://api.github.com/repos/tosuapp/tosu/releases/tags/v${context.currentVersion}`
            );
            const json = (await request.json()) as any;
            const {
                assets
            }: {
                assets: {
                    name: string;
                    digest: `${string}:${string}`;
                    browser_download_url: string;
                }[];
            } = json;

            const findAsset = assets.find((r) => isOverlayAsset(r.name));
            if (!findAsset) {
                await rm(gameOverlayPath, { recursive: true, force: true });
                throw new Error(
                    `Could not find downloadable files for your operating system. (${platform.type})` +
                        (isLinux
                            ? ` Build it with "pnpm build:overlay:linux" and copy packages/ingame-overlay/pack/linux-unpacked to ${gameOverlayPath}`
                            : '')
                );
            }

            await downloadFile(findAsset.browser_download_url, archivePath);

            const verify = await verifyDownload(findAsset.digest, archivePath);
            if (verify === false) {
                await rm(archivePath);
                return new Error('Unable to verify downloaded executable');
            }

            await unzip(archivePath, gameOverlayPath);
            await rm(archivePath);
            if (isLinux) await restoreLinuxPermissions(gameOverlayPath);

            wLogger.info('In-game overlay installed successfully');
        }

        wLogger.warn(`Launching in-game overlay...`);
        if (isLinux) logLinuxLaunchHint(gameOverlayPath);

        const child = spawn(path.join(gameOverlayPath, overlayExecutable), [], {
            detached: false,
            // 'overlapped' is a Windows-only pipe mode
            stdio: isLinux
                ? ['ignore', 'pipe', 'pipe', 'ipc']
                : ['ignore', 'overlapped', 'overlapped', 'ipc'],
            windowsHide: true,
            shell: false,
            env: {
                // Force nvidia optimus to prefer dedicated gpu
                SHIM_MCCOMPAT: '0x800000001',
                // Force ipc fd to 3 just in case
                NODE_CHANNEL_FD: '3',
                ...process.env
            }
        }) as ChildProcessByStdio<null, Readable, Readable>;
        if (!child.channel) {
            throw new Error(`Could not spawn overlay process with Ipc`);
        }

        child.stdout.setEncoding('utf-8').on('data', (data: string) => {
            // overlay logs are a bit verbose, so redirect them to debug log
            data = data.trim();

            if (data.startsWith('info:'))
                wLogger.info(data.replace('info:', '').trim());
            else if (data.startsWith('warn:'))
                wLogger.warn(data.replace('warn:', '').trim());
            else wLogger.debug(`Overlay Output:`, data.trim());
        });

        child.stderr.setEncoding('utf-8').on('data', (data: string) => {
            // redirect overlay error backtraces to debug error log
            wLogger.error('Overlay Error:', data.trim());
        });

        return child;
    } catch (error) {
        return error as Error;
    }
}
