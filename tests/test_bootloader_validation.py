"""Run the project's boot-validation functions with real nrfutil signatures.

The host harness extracts the current validation and micro-ecc backend bodies;
it does not replace their decisions. Only device-address arguments are widened
to uintptr_t and platform initialization/logging is shimmed. This verifies the
signature chain and failure propagation, not Cortex-M interrupts or stack use.
Run separately: python tests/test_bootloader_validation.py
"""
from pathlib import Path
import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'SDK/17.1.0_ddde560'
VALIDATION = ROOT / 'Bootloader/nrf_dfu_validation.c'
BACKEND = SDK / 'components/libraries/crypto/backend/micro_ecc/micro_ecc_backend_ecdsa.c'


def function_body(path, name):
    source = path.read_text(encoding='utf-8')
    match = re.search(r'(?m)^(?:static )?(?:bool|ret_code_t|nrf_dfu_result_t)\s+' + name + r'\s*\(', source)
    if not match:
        raise AssertionError('SDK function not found: ' + name)
    opening = source.index('{', match.end())
    depth = 1
    index = opening + 1
    while depth:
        depth += (source[index] == '{') - (source[index] == '}')
        index += 1
    return source[match.start():index]


def protobuf_fields(data):
    def varint(offset):
        result = shift = 0
        while True:
            value = data[offset]
            offset += 1
            result |= (value & 127) << shift
            if value < 128:
                return result, offset
            shift += 7
    fields = {}
    offset = 0
    while offset < len(data):
        tag, offset = varint(offset)
        kind = tag & 7
        if kind == 0:
            value, offset = varint(offset)
        elif kind == 2:
            size, offset = varint(offset)
            value = data[offset:offset + size]
            offset += size
        else:
            raise AssertionError('unexpected protobuf wire type')
        fields.setdefault(tag >> 3, []).append(value)
    return fields


SHIM = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#include "crc32.h"
#include "uECC.h"
#define NRF_LOG_INFO(...)
#define NRF_LOG_WARNING(...)
#define NRF_LOG_ERROR(...)
#define NRF_LOG_DEBUG(...)
#define NRF_LOG_HEXDUMP_DEBUG(...)
#define NRF_LOG_FLUSH(...)
#define ASSERT(...) ((void)0)
#define NRF_DFU_DEBUG 0
#define NRF_CRYPTO_HASH_SIZE_SHA256 32
#define NRF_CRYPTO_ECDSA_SECP256R1_SIGNATURE_SIZE 64
#define NRF_CRYPTO_ECC_RAW_PRIVATE_KEY_MAX_SIZE 32
#define NRF_CRYPTO_ECDSA_SIGNATURE_MAX_SIZE 64
#define NRF_ERROR_CRYPTO_ECDSA_INVALID_SIGNATURE 1
#define ECC_BACKEND_SWAP_BYTES uECC_VLI_NATIVE_LITTLE_ENDIAN
#define MIN(a,b) ((a)<(b)?(a):(b))
#define EXT_ERR(value) (64 + (value))
enum { NRF_DFU_RES_CODE_SUCCESS=1, NRF_DFU_RES_CODE_OPERATION_FAILED=10,
       NRF_DFU_RES_CODE_INVALID_OBJECT=5, NRF_DFU_EXT_ERROR_SIGNATURE_MISSING=8,
       NRF_DFU_EXT_ERROR_WRONG_SIGNATURE_TYPE=9 };
enum { NO_VALIDATION, VALIDATE_CRC, VALIDATE_SHA256, VALIDATE_ECDSA_P256_SHA256 };
typedef int dfu_signature_type_t;
#define DFU_SIGNATURE_TYPE_ECDSA_P256_SHA256 0
typedef int nrf_dfu_result_t;
typedef struct { uint32_t type; uint8_t bytes[64]; } boot_validation_t;
typedef sha256_context_t nrf_crypto_hash_context_t;
typedef struct { size_t raw_private_key_size; } nrf_crypto_ecc_curve_info_t;
typedef struct { struct { nrf_crypto_ecc_curve_info_t const *p_info; } header;
                 uint32_t key[16]; } nrf_crypto_backend_micro_ecc_common_key_t;
typedef nrf_crypto_backend_micro_ecc_common_key_t nrf_crypto_ecc_public_key_t;
typedef struct { int unused; } nrf_crypto_ecdsa_verify_context_t;
static nrf_crypto_ecc_curve_info_t curve_info = {32};
static nrf_crypto_ecc_public_key_t m_public_key = {{&curve_info},{0}};
static uint8_t m_signature[64], m_sig_hash[32], m_fw_hash[32];
static int g_nrf_crypto_hash_sha256_info, failure_mode;
static void crypto_init(void) {}
static void nrf_crypto_internal_swap_endian(uint8_t *dst, uint8_t const *src, size_t len) {
    for (size_t i=0; i<len; ++i) dst[i]=src[len-1-i];
}
static void nrf_crypto_internal_double_swap_endian(uint8_t *dst, uint8_t const *src, size_t len) {
    nrf_crypto_internal_swap_endian(dst,src,len);
    nrf_crypto_internal_swap_endian(dst+len,src+len,len);
}
static void nrf_crypto_internal_double_swap_endian_in_place(uint8_t *data, size_t len) {
    uint8_t temp[64]; memcpy(temp,data,2*len);
    nrf_crypto_internal_double_swap_endian(data,temp,len);
}
static ret_code_t nrf_crypto_hash_calculate(nrf_crypto_hash_context_t *ctx, void const *info,
 uint8_t const *data, size_t len, uint8_t *hash, size_t *hash_len) {
    if (failure_mode == 1 || *hash_len != 32) return 1;
    ret_code_t result=sha256_init(ctx);
    if (!result) result=sha256_update(ctx,data,len);
    if (!result) result=sha256_final(ctx,hash,false);
    *hash_len=32; return result;
}
static uECC_Curve nrf_crypto_backend_micro_ecc_curve_get(void const *key) { return uECC_secp256r1(); }
'''

ADAPTER = r'''
static ret_code_t nrf_crypto_ecdsa_verify(nrf_crypto_ecdsa_verify_context_t *ctx,
 nrf_crypto_ecc_public_key_t const *key, uint8_t const *hash, size_t hash_len,
 uint8_t const *signature, size_t signature_len) {
    if (failure_mode == 2 || signature_len != 64) return 1;
    return nrf_crypto_backend_micro_ecc_verify(ctx,key,hash,hash_len,signature);
}
'''

MAIN = r'''
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *file=fopen(argv[1],"rb"); if (!file) return 3;
    uint32_t lengths[2]; uint8_t init_sig[64]; boot_validation_t validation={VALIDATE_ECDSA_P256_SHA256,{0}};
    if (fread(lengths,4,2,file)!=2 || fread(m_public_key.key,1,64,file)!=64 ||
        fread(validation.bytes,1,64,file)!=64 || fread(init_sig,1,64,file)!=64) return 4;
    uint8_t *init=malloc(lengths[1]), *app=malloc(lengths[0]);
    if (!init || !app || fread(init,1,lengths[1],file)!=lengths[1] ||
        fread(app,1,lengths[0],file)!=lengths[0]) return 5;
    fclose(file);
    char const *test=argv[2]; bool result=false;
    if (!strncmp(test,"init_",5)) {
        uint8_t const *signature=init_sig; uint32_t sig_len=64; int type=0;
        if (!strcmp(test,"init_image_tamper")) init[lengths[1]/2]^=1;
        if (!strcmp(test,"init_signature_tamper")) init_sig[32]^=1;
        if (!strcmp(test,"init_missing")) signature=NULL;
        if (!strcmp(test,"init_wrong_type")) type=1;
        if (!strcmp(test,"init_wrong_length")) sig_len=63;
        result=nrf_dfu_validation_signature_check(type,signature,sig_len,init,lengths[1])==NRF_DFU_RES_CODE_SUCCESS;
    } else {
        if (!strncmp(test,"crc_",4)) {
            validation.type=VALIDATE_CRC;
            uint32_t crc=crc32_compute(app,lengths[0],NULL); memcpy(validation.bytes,&crc,4);
        }
        if (!strncmp(test,"sha_",4)) {
            validation.type=VALIDATE_SHA256; sha256_context_t ctx;
            sha256_init(&ctx); sha256_update(&ctx,app,lengths[0]); sha256_final(&ctx,validation.bytes,false);
        }
        if (strstr(test,"image_tamper")) app[lengths[0]/2]^=1;
        if (!strcmp(test,"ecdsa_signature_tamper")) validation.bytes[32]^=1;
        if (!strcmp(test,"ecdsa_zero_signature")) memset(validation.bytes,0,64);
        if (!strcmp(test,"ecdsa_public_key_tamper")) ((uint8_t*)m_public_key.key)[0]^=1;
        if (!strcmp(test,"ecdsa_hash_failure")) failure_mode=1;
        if (!strcmp(test,"ecdsa_verify_failure")) failure_mode=2;
        if (!strcmp(test,"unknown_type")) validation.type=255;
        if (!strcmp(test,"no_validation")) validation.type=NO_VALIDATION;
        result=nrf_dfu_validation_boot_validate(&validation,(uintptr_t)app,lengths[0]);
    }
    printf("%d\n",result); free(init); free(app); return 0;
}
'''


class BootloaderValidationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = 'D:/cygwin64/bin/gcc.exe' if os.name == 'nt' else shutil.which('gcc')
        cls.nrfutil = ROOT / ('tools/windows_tools/nrfutil.exe' if os.name == 'nt' else 'tools/nrfutil/nrfutil')
        if not cls.compiler or not Path(cls.compiler).is_file() or not cls.nrfutil.is_file():
            raise unittest.SkipTest('host GCC and packaged nrfutil are required')
        (ROOT / 'output').mkdir(exist_ok=True)
        cls.directory = tempfile.TemporaryDirectory(prefix='bootloader-validation-test-', dir=ROOT / 'output')
        cls.work = Path(cls.directory.name)
        cls.environment = dict(os.environ)
        if os.name == 'nt':
            cls.environment['PATH'] = 'D:/cygwin64/bin;' + cls.environment['PATH']
        package = os.environ.get('BOOTLOADER_VALIDATION_PACKAGE')
        if not package:
            app = cls.work / 'application.bin'
            app.write_bytes(bytes(range(256)) * 2)
            package = cls.work / 'ecdsa.zip'
            cls.run_command([str(cls.nrfutil), 'pkg', 'generate', '--application', str(app),
                     '--application-version', '61', '--hw-version', '52', '--sd-req', '0x0103',
                     '--key-file', str(ROOT / 'tools/priv.pem'), '--app-boot-validation',
                     'VALIDATE_ECDSA_P256_SHA256', str(package)])
        with zipfile.ZipFile(package) as archive:
            app = archive.read(next(name for name in archive.namelist() if name.endswith('.bin')))
            packet = protobuf_fields(archive.read(next(name for name in archive.namelist() if name.endswith('.dat'))))
        signed = protobuf_fields(packet[2][0])
        init = protobuf_fields(signed[1][0])[2][0]
        boot_validation = protobuf_fields(protobuf_fields(init)[10][0])
        if boot_validation[1][0] != 3:
            raise AssertionError('fixture must use ECDSA application boot validation')
        pk_source = (SDK / 'dfu/dfu_public_key.c').read_text(encoding='utf-8')
        pk = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{2})', pk_source))
        cls.fixture = cls.work / 'fixture.bin'
        cls.fixture.write_bytes(struct.pack('<II', len(app), len(init)) + pk + boot_validation[2][0] + signed[3][0] + init + app)
        cls.source = cls.work / 'validation_host.c'
        backend = function_body(BACKEND, 'nrf_crypto_backend_micro_ecc_verify')
        signature = function_body(VALIDATION, 'nrf_dfu_validation_signature_check')
        hash_check = function_body(VALIDATION, 'nrf_dfu_validation_hash_ok').replace('uint32_t src_addr', 'uintptr_t src_addr')
        boot = function_body(VALIDATION, 'nrf_dfu_validation_boot_validate').replace('uint32_t data_addr', 'uintptr_t data_addr')
        cls.source.write_text(SHIM + backend + ADAPTER + signature + hash_check + boot + MAIN, encoding='utf-8')
        (cls.work / 'sdk_errors.h').write_text('typedef unsigned int ret_code_t;\n#define NRF_SUCCESS 0\n#define NRF_ERROR_NULL 4\n', encoding='ascii')
        (cls.work / 'sdk_common.h').write_text(
            '#include <string.h>\n#define NRF_MODULE_ENABLED(module) 1\n'
            '#define VERIFY_PARAM_NOT_NULL(value) do { if (!(value)) return NRF_ERROR_NULL; } while (0)\n',
            encoding='ascii')
        cls.program = cls.work / ('validation_host.exe' if os.name == 'nt' else 'validation_host')
        ecc = SDK / 'external/micro-ecc/micro-ecc'
        cls.run_command([str(cls.compiler), '-std=c99', '-O2', '-DuECC_PLATFORM=0', '-DuECC_WORD_SIZE=4',
                 '-DuECC_OPTIMIZATION_LEVEL=2', '-DuECC_VLI_NATIVE_LITTLE_ENDIAN=1',
                 '-DuECC_SUPPORTS_secp160r1=0', '-DuECC_SUPPORTS_secp192r1=0',
                 '-DuECC_SUPPORTS_secp224r1=0', '-DuECC_SUPPORTS_secp256k1=0',
                 '-I' + cls.work.as_posix(), '-I' + (SDK / 'components/libraries/sha256').as_posix(),
                 '-I' + (SDK / 'components/libraries/crc32').as_posix(), '-I' + ecc.as_posix(),
                 cls.source.as_posix(), (ecc / 'uECC.c').as_posix(),
                 (SDK / 'components/libraries/sha256/sha256.c').as_posix(),
                 (SDK / 'components/libraries/crc32/crc32.c').as_posix(), '-o', cls.program.as_posix()])

    @classmethod
    def tearDownClass(cls):
        if (ROOT / 'output').resolve() not in cls.work.resolve().parents:
            raise AssertionError('temporary test output escaped the workspace')
        cls.directory.cleanup()

    @classmethod
    def run_command(cls, command):
        result = subprocess.run(command, cwd=ROOT, env=cls.environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding='utf-8', errors='replace')
        if result.returncode:
            raise AssertionError(result.stdout)
        return result.stdout.strip()

    def test_valid_boot_and_init_signatures_and_hashes(self):
        for case in ('ecdsa_valid', 'init_valid', 'crc_valid', 'sha_valid', 'no_validation'):
            with self.subTest(case=case):
                self.assertEqual(self.run_command([str(self.program), str(self.fixture), case]), '1')

    def test_tampering_and_crypto_errors_are_rejected(self):
        for case in ('ecdsa_image_tamper', 'ecdsa_signature_tamper', 'ecdsa_zero_signature',
                     'ecdsa_public_key_tamper', 'ecdsa_hash_failure', 'ecdsa_verify_failure',
                     'init_image_tamper', 'init_signature_tamper', 'init_missing',
                     'init_wrong_type', 'init_wrong_length', 'crc_image_tamper',
                     'sha_image_tamper', 'unknown_type'):
            with self.subTest(case=case):
                self.assertEqual(self.run_command([str(self.program), str(self.fixture), case]), '0')


if __name__ == '__main__':
    unittest.main()
