// SPDX-License-Identifier: Apache-2.0
package vendor.lenovo.hardware.gametuning;

// Empty strings inherit the driver defaults. Choices apply to both graphics APIs.
@VintfStability
parcelable GraphicsOptions {
    String textureFiltering = "";
    String anisotropicFiltering = "";
    String mipmapLodBias = "";
    String msaaLimit = "";
}
