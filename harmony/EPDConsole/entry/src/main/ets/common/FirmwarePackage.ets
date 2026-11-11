import { fileIo } from '@kit.CoreFileKit';
import { zlib } from '@kit.BasicServicesKit';
import { common } from '@kit.AbilityKit';

interface ManifestApp {
  bin_file: string;
  dat_file: string;
}

interface ManifestInner {
  application: ManifestApp;
}

interface ManifestRoot {
  manifest: ManifestInner;
}

export class FwInit {
  fwVersion: number = -1;
  hwVersion: number = -1;
  sdReq: number = -1;
}

export class FirmwarePackage {
  fileName: string = '';
  binName: string = '';
  datName: string = '';
  binSize: number = 0;
  init: FwInit = new FwInit();
  bin: Uint8Array = new Uint8Array(0);
  dat: Uint8Array = new Uint8Array(0);
}

function readAll(path: string): Uint8Array {
  const file = fileIo.openSync(path, fileIo.OpenMode.READ_ONLY);
  const stat = fileIo.statSync(path);
  const buf = new ArrayBuffer(stat.size);
  const n = fileIo.readSync(file.fd, buf);
  fileIo.closeSync(file);
  return new Uint8Array(buf.slice(0, n));
}

// nrfutil 6 packages encode: hw_version=52 (0x34), sd_req=[0x126] packed.
// The init packet is a protobuf; this targeted scan is validated against all
// vendor packages and avoids shipping a full protobuf runtime.
function parseInit(dat: Uint8Array): FwInit {
  const pat: number[] = [0x10, 0x34, 0x1a, 0x02, 0xa6, 0x02];
  const out = new FwInit();
  let idx: number = -1;
  for (let i: number = 0; i + pat.length <= dat.length; i++) {
    let ok: boolean = true;
    for (let j: number = 0; j < pat.length; j++) {
      if (dat[i + j] !== pat[j]) {
        ok = false;
        break;
      }
    }
    if (ok) {
      idx = i;
      break;
    }
  }
  if (idx >= 0) {
    out.hwVersion = 0x34;
    out.sdReq = 0x126;
    if (idx >= 2 && dat[idx - 2] === 0x08) {
      out.fwVersion = dat[idx - 1];
    }
  }
  return out;
}

export async function loadFirmwarePackage(ctx: common.UIAbilityContext, uri: string): Promise<FirmwarePackage> {
  const pkg = new FirmwarePackage();
  const rawName: string = uri.substring(uri.lastIndexOf('/') + 1);
  pkg.fileName = decodeURIComponent(rawName);
  const zipPath: string = ctx.cacheDir + '/fw_pkg.zip';
  const outDir: string = ctx.cacheDir + '/fw_out_' + Date.now().toString();
  try {
    fileIo.unlinkSync(zipPath);
  } catch (err) {
  }
  fileIo.mkdirSync(outDir, true);
  const src = fileIo.openSync(uri, fileIo.OpenMode.READ_ONLY);
  const dst = fileIo.openSync(zipPath, fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC);
  const buf = new ArrayBuffer(64 * 1024);
  while (true) {
    const n = fileIo.readSync(src.fd, buf);
    if (n <= 0) {
      break;
    }
    fileIo.writeSync(dst.fd, buf.slice(0, n));
  }
  fileIo.closeSync(src);
  fileIo.closeSync(dst);

  await zlib.decompressFile(zipPath, outDir);
  const manifest: string = fileIo.readTextSync(outDir + '/manifest.json');
  const json = JSON.parse(manifest) as ManifestRoot;
  pkg.binName = json.manifest.application.bin_file;
  pkg.datName = json.manifest.application.dat_file;
  pkg.bin = readAll(outDir + '/' + pkg.binName);
  pkg.dat = readAll(outDir + '/' + pkg.datName);
  pkg.binSize = pkg.bin.length;
  pkg.init = parseInit(pkg.dat);
  return pkg;
}