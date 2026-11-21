// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ortho_tile_builder.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ds::terrain::detail {
namespace {

std::atomic<std::uint64_t> nextSerial{1};

}  // namespace

std::array<std::uint8_t, 256> colourTable(const OrthoColor& c) {
    std::array<std::uint8_t, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        double v = i / 255.0;
        v = (v - 0.5) * c.contrast + 0.5 + c.brightness;
        v = std::clamp(v, 0.0, 1.0);
        v = std::pow(v, 1.0 / c.gamma);
        lut[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(v * 255.0));
    }
    return lut;
}

std::shared_ptr<OrthoTile> finishOrthoTile(const OrthoTileKey& key, int n,
                                           std::span<const float> alpha,
                                           std::span<const std::array<float, 3>> colour,
                                           const std::array<std::uint8_t, 256>* lut) {
    const int ext = n + 2 * kFeatherPx;
    const auto extN = static_cast<std::size_t>(ext);
    // Soft edges: inside the imagery, alpha ramps up over kFeatherPx texels (box filter of
    // the validity); outside it stays 0.
    auto out = std::make_shared<OrthoTile>();
    out->key = key;
    out->sizePx = n;
    out->rgba.resize(OrthoTile::bytesFor(n));
    const int box = 2 * kFeatherPx + 1;
    bool visible = false;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const std::size_t t = static_cast<std::size_t>(j + kFeatherPx) * extN +
                                  static_cast<std::size_t>(i + kFeatherPx);
            std::uint8_t* o =
                &out->rgba[(static_cast<std::size_t>(j) * static_cast<std::size_t>(n) +
                            static_cast<std::size_t>(i)) *
                           4U];
            if (alpha[t] == 0.0F) {
                o[0] = o[1] = o[2] = o[3] = 0;
                continue;
            }
            float sum = 0.0F;
            for (int b = 0; b < box; ++b) {
                for (int a = 0; a < box; ++a) {
                    sum += alpha[static_cast<std::size_t>(j + b) * extN +
                                 static_cast<std::size_t>(i + a)];
                }
            }
            // Share of valid texels around: 1 inside, ≈ 0.6 on an edge ⇒ 1/3, 2/3, 1.
            const float share = sum / static_cast<float>(box * box);
            const float soft = std::clamp((share - 0.4F) / 0.6F, 0.0F, 1.0F);
            for (std::size_t ch = 0; ch < 3; ++ch) {
                auto c8 =
                    static_cast<std::uint8_t>(std::lround(std::clamp(colour[t][ch], 0.0F, 255.0F)));
                if (lut != nullptr) {
                    c8 = (*lut)[c8];
                }
                o[ch] = static_cast<std::uint8_t>(std::lround(static_cast<float>(c8) * soft));
            }
            o[3] = static_cast<std::uint8_t>(std::lround(soft * 255.0F));
            visible = visible || o[3] > 0;
        }
    }
    if (!visible) {
        return nullptr;
    }
    // Mip levels: 2 × 2 box filter of the premultiplied colours.
    for (int m = 1; m < out->mipCount(); ++m) {
        const int src = std::max(n >> (m - 1), 1);
        const int dst = std::max(n >> m, 1);
        const std::uint8_t* s = out->rgba.data() + out->mipOffset(m - 1);
        std::uint8_t* t = out->rgba.data() + out->mipOffset(m);
        for (int j = 0; j < dst; ++j) {
            for (int i = 0; i < dst; ++i) {
                for (int ch = 0; ch < 4; ++ch) {
                    int sum = 0;
                    for (int b = 0; b < 2; ++b) {
                        for (int a = 0; a < 2; ++a) {
                            const int si = std::min(2 * i + a, src - 1);
                            const int sj = std::min(2 * j + b, src - 1);
                            sum += s[(static_cast<std::size_t>(sj) * static_cast<std::size_t>(src) +
                                      static_cast<std::size_t>(si)) *
                                         4U +
                                     static_cast<std::size_t>(ch)];
                        }
                    }
                    t[(static_cast<std::size_t>(j) * static_cast<std::size_t>(dst) +
                       static_cast<std::size_t>(i)) *
                          4U +
                      static_cast<std::size_t>(ch)] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
            }
        }
    }
    out->serial = nextSerial.fetch_add(1);
    return out;
}

}  // namespace ds::terrain::detail
