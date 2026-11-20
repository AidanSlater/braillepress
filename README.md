# Open Braille Embosser

C++20 tools that convert TXT, DOCX, PDF, raster images, and MusicXML/MXL to
Unicode Braille (`.brl`) or vector-dot PDF. The UNO Q Linux service holds
browser, Qt sender, and IPP jobs for review. Physical embossing is pending.

## Quickstart

```sh
docker build --network host -f Dockerfile.verify \
  --build-arg "VERIFY_USER=$(id -u):$(id -g)" -t open-braille-check .
docker run --rm --network host --user "$(id -u):$(id -g)" -v "$PWD:/work" \
  open-braille-check ./build/braille tests/fixtures/simple.txt /work/sample.brl
```

The build runs CTest and the demo on Debian 13. Add `--build-arg BASE=ubuntu:24.04`
for Ubuntu or `--build-arg SANITIZE=ON` for ASan/UBSan.

<a name="spanish"></a>

Use `--ocr` for scans, `--language es` for Spanish Braille and OCR, and `--help`
for all converter options. [ONCE B 2](https://www.once.es/servicios-sociales/braille/comision-braille-espanola/documentos-tecnicos/documentos-tecnicos-relacionados-con-el-braille/documentos/b2-signografia-basica-de-las-lenguas-cooficiales-espanolas-v4-2026-01-22.pdf)

## UNO Q actuator bench

Open Braille Bench is a C++/Qt app for testing the UNO Q. It supports X/Y
STEP/DIR jogs, single-pin PWM, six PCA9685 servos, a timed GPIO press trigger,
and digital switch monitoring. It opens in simulation mode and reaches the
MCU through an SSH tunnel in board mode. The print service uses the dry-run
job protocol.
[Board research, Docker builds, and laptop setup](firmware/uno_q/README.md)

## Service and build options

- Use `./build/braille-service --state /work/braille-jobs` as the Docker command
  and open `http://127.0.0.1:8000`. [Service image](Dockerfile.native) · [UNO Q deployment](cmake/DeployUnoQ.cmake)
- For native builds, `BRAILLE_DESKTOP=ON` enables the Qt sender.
  `BRAILLE_SERVICE=ON` needs `BRAILLE_PAPPL_ROOT` set to the patched PAPPL prefix.
  [CMake options](CMakeLists.txt) · [PAPPL build](cmake/BuildPappl.cmake)
- For printed scores, enable `BRAILLE_MUSIC_SCAN=ON` and point
  `BRAILLE_ONNXRUNTIME_ROOT` to the ONNX Runtime CPU SDK. Run with `--music`
  and `BRAILLE_OMR_MODELS` set to the homr 0.7.0 model directory.

## Test fixtures

The basic DOCX/PDF fixtures came from python-docx 1.2.0 and pypdf 6.21.2. Pillow
13.0.0 generated the graph PNGs. [Fixture data](tests/fixtures)

Project source is MIT. Dependency licenses also apply to binary distribution. [License](LICENSE)
