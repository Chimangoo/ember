#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Requires Python 3 or higher

import os, shutil, subprocess, tempfile, fnmatch, traceback, re, sys


def find_matches(files, patterns):
	matches = []
	for pattern in patterns:
		for file in fnmatch.filter(files, pattern):
			matches.append(file)
	return matches
	
def collect_licenses(src_assets_path, licenses, assets):
	"""Walk upwards looking for any license or author files"""
	for asset in assets:
		asset_path = os.path.join(src_assets_path, asset)
		if os.path.isfile(asset_path):
			asset_path_tokens = os.path.dirname(asset).split(os.sep)
			while len(asset_path_tokens) > 0:
				dir = (os.sep).join(asset_path_tokens)
				for file in find_matches(os.listdir(os.path.join(src_assets_path, dir)), ["*license*", "*LICENSE*", "*authors*", "*AUTHORS*"]):
					licenses.add(os.path.join(dir, file))
				del asset_path_tokens[-1]
				


def copy_assets(src_assets_path, dest_assets_path, assets, image_max_size):
	copied = 0
	skipped = 0
	converted = 0
	errors = 0
	for asset in assets:
		asset_path = os.path.join(src_assets_path, asset)
		if os.path.isfile(asset_path):
			dest_asset_path = os.path.join(dest_assets_path, asset)
			
			dest_asset_dir_path = os.path.dirname(dest_asset_path)
			if not os.path.exists(dest_asset_dir_path):
				os.makedirs(dest_asset_dir_path)
			#Check if the destination file exists, and if so if it's older than the source
			if os.path.exists(dest_asset_path):
				if os.path.getmtime(dest_asset_path) >= os.path.getmtime(asset_path):
					#destination file is newer or of the same date as the soruce file
					skipped = skipped + 1
					continue
				
			if asset.endswith(".png") and not asset.startswith("common"):
				#if it's an image we should scale it, unless it's an image in the "common" directory
				colourdirective = ""
				imagemetadata = subprocess.check_output(["identify", asset_path])
				if "256c" in imagemetadata.decode("utf-8"):
					 colourdirective = "-colors 256"
					 
				print("converting image asset {0} to {1}".format(asset, dest_asset_path))
				convert_cmd = ["convert", asset_path, "-quality 95 -depth 8", colourdirective, '-resize "{0}x{0}>"'.format(image_max_size), dest_asset_path]
				#print(" ".join(convert_cmd))
				if os.system(" ".join(convert_cmd)) == 0:
					converted = converted + 1
				else:
					errors = errors + 1
				#subprocess.call(convert_cmd, stderr=subprocess.STDOUT)
			else:
				print("copying asset {0} to {1}".format(asset, dest_asset_path))
				shutil.copy(asset_path, dest_asset_path)
				copied = copied + 1

			
		else:
			errors = errors + 1
			print("referenced file {0} does not exist".format(asset_path))
		
	return copied, converted, skipped, errors
		
def copytree(src, dst):
    if not os.path.exists(dst):
        os.makedirs(dst)
    for item in os.listdir(src):
        s = os.path.join(src, item)
        d = os.path.join(dst, item)
        if os.path.isdir(s):
            copytree(s, d)
        else:
            if not os.path.exists(d) or os.stat(src).st_mtime - os.stat(dst).st_mtime > 1:
                shutil.copy2(s, d)	

def main():
	"""The main entry point."""
	usage = """usage: {0} <MEDIA_DIRECTORY_TRUNK> <OUTPUT_DIRECTORY> <IMAGE_MAX_SIZE>

This script will automatically scan the media source repo and create a repository suitable for distributions.
This process mainly involves copying only those assets which are used by Ember, and resizing textures.


MEDIA_DIRECTORY_TRUNK is the path to the root of the media directory, often named "trunk"

OUTPUT_DIRECTORY is where the processed assets will be copied

IMAGE_MAX_SIZE the max size in pixels of any image. Any larger image will be scaled

OPTIONS
	-h Show this help text
""".format(sys.argv[0])

	if len(sys.argv) == 1:
		print("ERROR: MEDIA_DIRECTORY_TRUNK must be specified!")
		print(usage)
		sys.exit(1)
	elif len(sys.argv) == 2:
		print("ERROR: OUTPUT_DIRECTORY must be specified!")
		print(usage)
		sys.exit(1)
	elif len(sys.argv) == 3:
		print("ERROR: IMAGE_MAX_SIZE must be specified!")
		print(usage)
		sys.exit(1)

	if sys.argv[1] == "-h":
		print(usage)
		sys.exit(0)

	mode = sys.argv[1]
	startdir = os.getcwd()
	src_media_dir = sys.argv[1]
	src_assets_dir = os.path.join(src_media_dir, "assets")
	#Place the resulting media in a subdirectory called "media"
	dest_media_dir = os.path.join(sys.argv[2], "media")
	dest_assets_dir = os.path.join(dest_media_dir, "assets")
	
	if not os.path.exists(dest_media_dir):
		os.makedirs(dest_media_dir)
	
	#First copy all directories that we should just provide unchanged
	copytree(os.path.join(src_media_dir, "assets_external/caelum"), os.path.join(dest_media_dir, "assets_external/caelum"))
	#Copy license files
	shutil.copy(os.path.join(src_media_dir, "LICENSING.txt"), dest_media_dir)
	shutil.copy(os.path.join(src_media_dir, "COPYING.txt"), dest_media_dir)
	
	#Now walk through all files in assets looking for references to textures etc.
	reTextureAlias = re.compile(r"^[^/].*set_texture_alias [\w]* (.*)") 
	reTexture = re.compile(r"^[^/].*texture [\w]* (.*)") 
	reParticleImage = re.compile(r".*image\s*(.*)") 
	reIcon = re.compile(r'.*icon=\"([^\"]*)\"')
	reMesh = re.compile(r'.*mesh=\"([^\"]*)\"')
	reDiffuse = re.compile(r'.*diffusetexture=\"([^\"]*)\"')
	reNormal = re.compile(r'.*normalmaptexture=\"([^\"]*)\"')
	reFilename = re.compile(r'.*filename=\"([^\"]*)\"')
	
	
	#Put all assets here, as paths relative to the "assets" directory in the media repo.
	assets = set();
	
	#Parse .material and .particle files, looking for references to textures. Skip "source" directories.
	for dirpath, dirnames, files in os.walk(src_assets_dir):
		if "source" in dirnames:
			dirnames.remove("source")
		for filename in fnmatch.filter(files, "*.material"):
			filepath = os.path.join(dirpath, filename)
			assets.add(os.path.relpath(filepath, src_assets_dir))
			file = open(filepath) 
			for line in file:
				aMatch = reTextureAlias.match(line)
				if aMatch:
					assets.add(aMatch.group(1))
					continue
				aMatch = reTexture.match(line)
				if aMatch:
					assets.add(aMatch.group(1))
					continue
		for filename in fnmatch.filter(files, "*.particle"):
			filepath = os.path.join(dirpath, filename)
			assets.add(os.path.relpath(filepath, src_assets_dir))
			file = open(filepath) 
			for line in file:
				aMatch = reParticleImage.match(line)
				if aMatch:
					assets.add(aMatch.group(1))
					continue
		for filename in fnmatch.filter(files, "*.skeleton"):
			assets.add(os.path.relpath(os.path.join(dirpath, filename), src_assets_dir)) 

	#Copy files found in the "common" assets. Skip "source" directories.
	for dirpath, dirnames, files in os.walk(os.path.join(src_assets_dir, "common")):
		if "source" in dirnames:
			dirnames.remove("source")
		for filename in files:
			if filename.endswith(('.jpg', '.png', '.skeleton', '.mesh', '.ttf', '.material', '.program', '.cg', '.glsl', '.hlsl', '.overlay', '.compositor', '.fontdef')):
				assets.add(os.path.relpath(os.path.join(dirpath, filename), src_assets_dir))
			
	#Parse .modeldef and .terrain files from the Ember source and look for references
	for dirpath, dirnames, files in os.walk(os.path.join(startdir, "data")):
		if "source" in dirnames:
			dirnames.remove("source")
		for filename in fnmatch.filter(files, "*.modeldef*"):
			file = open(os.path.join(dirpath, filename)) 
			for line in file:
				aMatch = reIcon.match(line)
				if aMatch:
					assets.add(aMatch.group(1))
					continue
				aMatch = reMesh.match(line)
				if aMatch:
					assets.add(aMatch.group(1))
					continue
		for filename in fnmatch.filter(files, "*.terrain*"):
			file = open(os.path.join(dirpath, filename)) 
			for line in file:
				aMatch = reDiffuse.match(line)
				if aMatch:
					assets.add(aMatch.group(1))
					continue
				aMatch = reNormal.match(line)
				if aMatch:
					assets.add(aMatch.group(1))
					continue
		for filename in fnmatch.filter(files, "*.sounddef*"):
			file = open(os.path.join(dirpath, filename)) 
			for line in file:
				aMatch = reFilename.match(line)
				if aMatch:
					assets.add(aMatch.group(1))
					continue

	


	licenses = set()
	collect_licenses(src_assets_dir, licenses, assets)
	
	assets.update(licenses)
		
	copied, converted, skipped, errors = copy_assets(src_assets_dir, dest_assets_dir, assets, 512)
	print("Media conversion completed.\n{0} files copied, {1} images converted, {2} files skipped, {3} with errors".format(copied, converted, skipped, errors))

	pass

if __name__ == '__main__':
	main()