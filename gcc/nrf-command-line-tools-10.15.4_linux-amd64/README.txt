This is an installation guide on how to install NRFTools on linux

NRFTools requires that the J-Link Software and Documentation Pack is installed.
It can be installed with the provided JLink_Linux packages, or downloaded from https://www.segger.com/downloads/jlink.

Install NRFTools with deb package:
sudo dpkg -i --force-overwrite nRFTools_<version_number>_Linux.deb
Or open with your preferred package manager.

Install NRFTools with tarball:
1) Copy the folders nrfjprog and mergehex to /opt

2) Make symbolic link for nrfjprog in /usr/local/bin
	sudo ln -s /opt/nrfjprog/nrfjprog /usr/local/bin/nrfjprog

3) Make symbolic link for mergehex in /usr/local/bin
	sudo ln -s /opt/mergehex/mergehex /usr/local/bin/mergehex
