#
# SPDX-License-Identifier: Apache-2.0
#

# PowerManagerService owns DT2W; the Lineage hardware UI owns high polling.
$(call soong_config_set,qtipower,mode_ext_lib,//hardware/lenovo:power-ext-lenovo)

# Device makefiles select the touch packages and Soong capabilities.
