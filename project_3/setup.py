from setuptools import Extension, setup

setup(
    name="faststats",
    version="1.0",
    py_modules=[],
    ext_modules=[
        Extension(
            "faststats",
            sources=["faststats.c"],
            extra_compile_args=["-O3", "-Wall", "-Wextra", "-ffp-contract=off"],
        )
    ],
)
