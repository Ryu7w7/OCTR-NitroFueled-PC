# Configuration file for the Sphinx documentation builder.
#
# This file only contains a selection of the most common options. For a full
# list see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

import re


html_theme = "rocm_docs_theme"
html_theme_options = {"flavor": "rocm"}

extensions = ["rocm_docs", "rocm_docs.doxygen"]
external_toc_path = "./sphinx/_toc.yml"
doxygen_root = "doxygen"
doxysphinx_enabled = True
doxygen_project = {
    "name": "half",
    "path": "doxygen/xml",
}

with open('../CMakeLists.txt', encoding='utf-8') as f:
    match = re.search(r'rocm_setup_version\(VERSION\s+([0-9.]+)', f.read())
    if not match:
        raise ValueError("VERSION not found!")
    version_number = match[1]

version = version_number
release = version_number
html_title = f"half {version}"
project = "half"
author = "Advanced Micro Devices, Inc."
copyright = (
    "Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved."
)
