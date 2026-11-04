// Jenkinsfile - multi-stage CI for the STM32 C/C++ (FreeRTOS) application.
//
//   Checkout -> Toolchain -> Build -> Static analysis -> Boot contract -> Image -> Publish
//
// Every build command goes through scripts/docker-dev.sh (DCON-001), so a
// Jenkins agent only needs git and Docker. The stm32-dev image is built from
// docker/Dockerfile on first use and then reused on that agent.
//
// The pipeline is declarative (Jenkins Pipeline 2.x). Run it from a
// Multibranch Pipeline job or a Pipeline job with "Pipeline script from SCM".

pipeline {
    agent any

    options {
        skipDefaultCheckout()
        timestamps()
        timeout(time: 45, unit: 'MINUTES')
        buildDiscarder(logRotator(numToKeepStr: '30', artifactNumToKeepStr: '10'))
        disableConcurrentBuilds()
    }

    parameters {
        choice(name: 'BUILD_TYPE', choices: ['Release', 'Debug'], description: 'CMake build type')
        booleanParam(name: 'RUN_LINT', defaultValue: true, description: 'Run clang-tidy')
        booleanParam(name: 'STRICT_LINT', defaultValue: false, description: 'Fail on any clang-tidy finding')
    }

    environment {
        STM32_IMAGE = 'stm32-dev:latest'
    }

    stages {
        stage('Checkout') {
            steps {
                checkout scm
                sh '''
                    git log -1 --pretty=fuller
                    echo "commit: $(git rev-parse HEAD)"
                    echo "branch: $(git rev-parse --abbrev-ref HEAD)"
                '''
            }
        }

        stage('Toolchain') {
            steps {
                sh '''
                    docker version --format 'docker server: {{.Server.Version}}'
                    if ! docker image inspect "${STM32_IMAGE}" >/dev/null 2>&1; then
                        echo "Building ${STM32_IMAGE} from docker/Dockerfile"
                        make image
                    fi
                    docker image inspect --format 'image: {{.Id}}' "${STM32_IMAGE}"
                    scripts/docker-dev.sh run arm-none-eabi-gcc --version | head -n 1
                    scripts/docker-dev.sh run cmake --version | head -n 1
                '''
            }
        }

        stage('Build') {
            steps {
                sh "make build BUILD_TYPE=${params.BUILD_TYPE}"
            }
        }

        stage('Static analysis') {
            when { expression { return params.RUN_LINT } }
            steps {
                script {
                    if (params.STRICT_LINT) {
                        sh 'make lint-strict'
                    } else {
                        sh 'make lint'
                    }
                }
            }
        }

        stage('Boot contract') {
            steps {
                sh '''
                    python3 - <<'PY'
from pathlib import Path

# Values shared with soccentric-stm32-bootloader (LAY-001..LAY-005, HND-003,
# Appendix B). If this stage fails, the application and BOOT have drifted.
expected = {
    "APP_SLOT_BASE": 0x0800C000,
    "APP_SLOT_SIZE": 0x000F4000,
    "APP_META_A_ADDR": 0x08008000,
    "APP_META_B_ADDR": 0x08108000,
    "APP_META_VERIFIED_OFFSET": 0x100,
    "APP_META_ACTIVATED_OFFSET": 0x120,
    "APP_META_CONFIRMED_OFFSET": 0x140,
    "APP_META_REJECTED_OFFSET": 0x160,
}

defines = {}
for line in Path("src/boot_contract.h").read_text(encoding="utf-8").splitlines():
    parts = line.split()
    if len(parts) >= 3 and parts[0] == "#define":
        defines[parts[1]] = parts[2]

for name, want in expected.items():
    if name not in defines:
        raise SystemExit(f"boot contract: {name} is missing from src/boot_contract.h")
    raw = defines[name].rstrip("ULul").rstrip("ULul")
    got = int(raw, 16)
    if got != want:
        raise SystemExit(f"boot contract: {name} is 0x{got:08X}, expected 0x{want:08X}")

print("boot contract: slot, metadata and record offsets match the BOOT layout")
PY
                '''
            }
        }

        stage('Image') {
            steps {
                sh '''
                    make size
                    python3 - build/demo.img <<'PY'
import struct
import sys
import zlib
from pathlib import Path

path = Path(sys.argv[1])
data = path.read_bytes()
header = struct.unpack_from("<IHHIIIIIII", data, 0)
magic, hver, hsize, itype, ver, isize, icrc, load, entry, _ts = header
assert magic == 0x48574249, "bad image magic"
assert hver == 1, f"unexpected header version {hver}"
assert hsize == 256, f"unexpected header size {hsize}"
assert itype == 1, f"unexpected image type {itype}"
assert load == 0x0800C000, f"image loads at 0x{load:08X}, expected 0x0800C000"
assert len(data) == hsize + isize, "image size does not match the header"
assert zlib.crc32(data[hsize:]) & 0xFFFFFFFF == icrc, "image CRC32 mismatch"
sp = struct.unpack_from("<I", data, hsize)[0]
assert 0x20000000 < sp <= 0x20080000, "initial stack pointer is outside internal RAM"
entry_ok = entry & 1 and 0x0800C000 <= (entry & ~1) < 0x0800C000 + 0x000F4000
assert entry_ok, "reset vector is outside the slot"
print(f"image: {path} v{(ver >> 24) & 0xFF}.{(ver >> 16) & 0xFF}.{ver & 0xFF} "
      f"({isize} bytes payload, entry 0x{entry:08X})")
PY
                '''
            }
        }

        stage('Publish') {
            steps {
                sh '''
                    sha256sum build/app.elf build/app.bin build/app.hex > build/SHA256SUMS
                    sha256sum build/demo.img build/app.map >> build/SHA256SUMS
                    cat build/SHA256SUMS
                '''
                archiveArtifacts artifacts: 'build/app.elf,build/app.bin,build/app.hex,build/demo.img,build/app.map,build/SHA256SUMS',
                                 fingerprint: true
            }
        }
    }

    post {
        always {
            deleteDir()
        }
        success {
            echo 'STM32 application: build, lint and image checks passed.'
        }
        failure {
            echo 'STM32 application: pipeline failed - see the stage logs above.'
        }
    }
}
