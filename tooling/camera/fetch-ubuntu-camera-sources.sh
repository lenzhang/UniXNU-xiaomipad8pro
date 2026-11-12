#!/bin/bash
# Fetch the Ubuntu piano kernel's camera files at the pinned commit into the ignored out/upstream/ (never committed): the source of
# data/camera's sensor tables (tooling/camera/sensor-tables.py extract / check --src) and the specification docs/drivers/camera.md
# cites. Each file is checked against its SHA-256 below; a mismatch is an error (the pin moved: change COMMIT and the hashes together,
# re-extract, review the diff of data/camera, regenerate data/camera/MANIFEST).
#   fetch-ubuntu-camera-sources.sh [--check]   --check: verify what out/upstream has, fetch nothing
#   environment: PIANO_UBUNTU_KERNEL=DIR       a local checkout of the same commit to copy from instead of the network (each file is
#                                              still checked against its pin)
# Prints the directory last; out/upstream/k is a link to it.
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
REPO=yzddmr6/linux-sm8750-piano
COMMIT=7f8d6bedde479a01f4a05d6d0581b43c7fd20a01   # piano/dev, "Merge the v1.2.0 kernel into piano/dev" (docs/ubuntu-history/kernel.md)
OUT=$D/out/upstream/linux-sm8750-piano-${COMMIT:0:12}
FILES=$(cat <<'EOF'
drivers/media/i2c/s5kjn1.c 4d1e2dad37d06fb594ac044903fe2eb674b3bf1ab3135da56913b53d6b244a92
drivers/media/i2c/ov32d40.c f784a23f088de3dccc165d61b7ebbce6484c4f7a7c0420cf901ede417ffba684
drivers/media/i2c/dw9768.c f1eafc295ba8a3daa5eac496eab57a9b41839511cb44a29b3ae10a9e3d4ecba4
drivers/i2c/busses/i2c-qcom-cci.c 9f62536ad0f0f0b4f0c802da79e047d2a9a9c7f5104a31655438b679e437b8c4
drivers/i2c/busses/i2c-qcom-geni.c 3a06de960fcfba83db656431b7465f49d40bc422caf0dfc19825de277568630d
arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano-camera.dtsi fb551d4e604587b323c25585f0472d4c8868ae9c8e44bcb95319636f97e5bbbd
arch/arm64/boot/dts/qcom/sm8750-xiaomi-piano.dts 79fe7c8009621e4fe00423a10218eacee6b92294ee978a5044026d0e05d127dd
arch/arm64/boot/dts/qcom/sm8750.dtsi b94834a8364d8007d5c12af1b5ad2e0a74a80094fcecb6a583a6d0d6b2ada923
drivers/clk/qcom/camcc-sm8750.c 191c1754ca0f1408cdd9e2e1c9caaa290b5f707633f7bb84e443dfde08f29b93
drivers/clk/qcom/cambistmclkcc-sm8750.c b9368cf617af714f47273b24f674677213a2b6189cf51ff02b61b9e7b7568dfa
drivers/clk/qcom/gcc-sm8750.c 0cefef94a558b310be2a903d9ad883b1226cc1897a7bed517b119b97794f39bd
drivers/clk/qcom/clk-alpha-pll.c d81f628af746854dbdd6ef8b0f2e36f85ba38858404690759199a026cac77109
drivers/clk/qcom/gdsc.c 04c66019948835d4b0def6f96d940500a85eb3e9c277f69de06c953f7d6475d2
drivers/pinctrl/qcom/pinctrl-sm8750.c 9772d7520ee7a6271d7fd7f3ecba58d3fe86073387569604d589d8c907d5ffff
drivers/pinctrl/qcom/pinctrl-msm.c d6638c03c09f5c0b24d9e343e19fb04ad7d483ec4f1c5fdabde4efc3570d59f8
drivers/media/platform/qcom/camss/camss.c f441f16d8ce04b1e7e5a45677d9c5f56a9dcaf139ad3eb4e90c00ba7e99be38c
drivers/media/platform/qcom/camss/camss-csid.c 2779a9a96469ed4e1e30da2802951b26f0626cba8eb2dac99594b0ba3badf1c2
drivers/media/platform/qcom/camss/camss-csid.h e8770350675dc69a7b8026d3b538ce992c4d726b6ce2835d87b02989935edcde
drivers/media/platform/qcom/camss/camss-csid-980.c 9135407e84aba9dc3ce8ac2b94713086ffa30ca54ba0154043a8042d86123635
drivers/media/platform/qcom/camss/camss-csid-gen4.c 289dfc0655e4d0df9707ca3c9cc06f001904046e279d2da0a7c4f64225d36295
drivers/media/platform/qcom/camss/camss-csiphy.c 9694bc604c745f17ebe7962752af510b3657030560b3597aabdde2c970da6eab
drivers/media/platform/qcom/camss/camss-csiphy.h b07b8ac6e5911c4d8a7f35e5a4937e4735d1845aca22622898cbab910cdbe51a
drivers/media/platform/qcom/camss/camss-csiphy-3ph-1-0.c 51c1101c69c5858d0a72cec74e810ea21c85561b170818ef786c198ce31ce12f
drivers/media/platform/qcom/camss/camss-vfe.c c31386986b475b49dfcf6f34310a5c79de57b1c9f9f84f675fde7656069a55dc
drivers/media/platform/qcom/camss/camss-vfe.h a088e4ea6491f1b17f453a5cd48bf97b0bebd65f41b07d4c091b431b4ef8bd04
drivers/media/platform/qcom/camss/camss-vfe-gen4.c 9213846876cc5781fd4c39f620aca135b6e24482b43c52035ae08602c66de7be
EOF
)
CHECK=0; [ "${1:-}" = --check ] && CHECK=1
bad=0
while read -r path want; do
  [ -n "$path" ] || continue
  dest=$OUT/$path
  if [ "$CHECK" = 0 ] && [ ! -f "$dest" ]; then
    mkdir -p "$(dirname "$dest")"
    if [ -n "${PIANO_UBUNTU_KERNEL:-}" ]; then
      cp "$PIANO_UBUNTU_KERNEL/$path" "$dest.part"
    else
      curl -fsS -m 120 -o "$dest.part" "https://raw.githubusercontent.com/$REPO/$COMMIT/$path"
    fi
    mv "$dest.part" "$dest"
  fi
  if [ ! -f "$dest" ]; then echo "missing: $dest (run without --check)" >&2; bad=1; continue; fi
  got=$(shasum -a 256 "$dest" | cut -d' ' -f1)
  if [ "$got" != "$want" ]; then echo "SHA-256 mismatch: $path: $got, pinned $want" >&2; bad=1; fi
done <<< "$FILES"
[ "$bad" = 0 ] || exit 1
ln -sfn "$(basename "$OUT")" "$D/out/upstream/k"
echo "$OUT"
