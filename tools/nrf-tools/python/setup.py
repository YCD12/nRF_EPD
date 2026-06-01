from setuptools import setup

setup(
package_data={
        'pynrfjprog.lib_x86': ['*.dll', '*.so*', '*.dylib*', 'jlinkarm_nrf_worker*'],
        'pynrfjprog.lib_armhf': ['*.dll', '*.so*', '*.dylib*', 'jlinkarm_nrf_worker*'],
        'pynrfjprog.lib_arm64': ['*.dll', '*.so*', '*.dylib*', 'jlinkarm_nrf_worker*'],
        'pynrfjprog.lib_x64': ['*.dll', '*.so*', '*.dylib*', 'jlinkarm_nrf_worker*'],
        'pynrfjprog.docs': ['*.h', 'nrfjprog_release_notes*.txt'],
        'pynrfjprog.examples': ['*.hex']
    }
)
