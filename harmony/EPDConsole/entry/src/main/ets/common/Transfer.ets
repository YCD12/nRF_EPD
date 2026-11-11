import { Cmd, buildBlockFrame, frame, missingBlocks, TransferStatus } from './Protocol';
import { BleClient } from './BleClient';

const MAX_RETRIES: number = 3;
const BATCH_SIZE: number = 20;
const BATCH_DELAY_MS: number = 150;

function delay(ms: number): Promise<void> {
  return new Promise<void>((resolve: () => void) => {
    setTimeout(() => {
      resolve();
    }, ms);
  });
}

export async function sendLayerCrc(client: BleClient, data: Uint8Array, isBw: boolean,
  mtu: number, onProgress: (sent: number, total: number) => void): Promise<void> {
  const layer: number = isBw ? 0x0F : 0x00;
  const chunkSize: number = Math.max(mtu - 8, 20);
  const totalBlocks: number = Math.ceil(data.length / chunkSize);
  const sessionId: number = Date.now() & 0xFF;

  await client.write(frame(Cmd.RESET_TRANSFER, new Uint8Array([sessionId])), true);
  await delay(100);

  for (let round: number = 0; round < MAX_RETRIES; round++) {
    let missing: number[] = [];
    if (round === 0) {
      for (let i: number = 0; i < totalBlocks; i++) {
        missing.push(i);
      }
    } else {
      const st: TransferStatus = await client.queryStatus();
      missing = missingBlocks(st, totalBlocks);
      if (missing.length === 0) {
        return;
      }
    }

    for (let i: number = 0; i < missing.length; i++) {
      const blockId: number = missing[i];
      const offset: number = blockId * chunkSize;
      const end: number = Math.min(offset + chunkSize, data.length);
      if (end <= offset) {
        continue;
      }
      const payload: Uint8Array = data.slice(offset, end);
      const cfg: number = (blockId === 0 ? 0x00 : 0xF0) | layer;
      const useResponse: boolean = ((i + 1) % BATCH_SIZE === 0) || (i === missing.length - 1);
      await client.write(buildBlockFrame(blockId, totalBlocks, cfg, payload), useResponse);
      onProgress(i + 1, missing.length);
    }

    await delay(BATCH_DELAY_MS);
  }

  const finalStatus: TransferStatus = await client.queryStatus();
  if (missingBlocks(finalStatus, totalBlocks).length > 0) {
    throw new Error('CRC 传输失败（重试后仍有缺失块）');
  }
}

export async function sendLayerLegacy(client: BleClient, data: Uint8Array, isBw: boolean,
  mtu: number, ackInterval: number, onProgress: (sent: number, total: number) => void): Promise<void> {
  const chunkSize: number = Math.max(mtu - 2, 20);
  const totalBlocks: number = Math.ceil(data.length / chunkSize);
  let noReplyCount: number = ackInterval;

  for (let i: number = 0; i < data.length; i += chunkSize) {
    const end: number = Math.min(i + chunkSize, data.length);
    const payload = new Uint8Array(1 + (end - i));
    payload[0] = (isBw ? 0x0F : 0x00) | (i === 0 ? 0x00 : 0xF0);
    payload.set(data.slice(i, end), 1);
    const withResponse: boolean = noReplyCount <= 0;
    await client.write(frame(Cmd.WRITE_IMG, payload), withResponse);
    noReplyCount = withResponse ? ackInterval : noReplyCount - 1;
    onProgress(Math.floor(i / chunkSize) + 1, totalBlocks);
  }
}
