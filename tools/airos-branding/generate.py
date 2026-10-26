#!/usr/bin/env python3
"""Regenerate every air/OS branding asset in the source tree.

Usage (from anywhere):
	python3 tools/airos-branding/generate.py [--boot-screen-tool PATH]

Needs Python 3 with pycairo, Pillow and numpy.  --boot-screen-tool is the
host build of src/tools/generate_boot_screen (jam '<build>generate_boot_screen');
without it the boot screen PNGs are written but
headers/private/kernel/boot/images-airos.h is left alone.

Everything is drawn from the geometry in airos_art.py, which was fitted to
the brand sheet (data/artwork/airos/brand-sheet.png).
"""

import argparse
import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
TOP = os.path.abspath(os.path.join(HERE, '..', '..'))

import cairo
import numpy as np
from PIL import Image

import airos_art as A
import hvif_render
import icons
import lockups
import rdef
import tga
import wallpaper

CLOUD_TEXT = (0xF6, 0xF3, 0xEA)


def path(*parts):
	return os.path.join(TOP, *parts)


def write(rel, data, mode='wb'):
	full = path(rel)
	os.makedirs(os.path.dirname(full), exist_ok=True)
	with open(full, mode) as f:
		f.write(data)
	print('wrote', rel)


def set_rdef(rel, header_regex, data, occurrence=0):
	full = path(rel)
	text = open(full).read()
	span = rdef.find_block(text, header_regex, occurrence)
	text = rdef.replace_blob(text, span, data)
	open(full, 'w').write(text)
	print('updated', rel, header_regex)


def png_bytes(surface):
	buf = io.BytesIO()
	surface.write_to_png(buf)
	return buf.getvalue()


# -- vector icons ---------------------------------------------------------

def vector_icons():
	base = lambda name: open(os.path.join(HERE, 'base', name), 'rb').read()
	out = {}
	out['app'] = icons.app_icon().data()
	# The Haiku boot disk and system folder, with the mark where the leaf was.
	out['boot-volume'] = icons.overlay(base('haiku-boot-volume.hvif'),
		[9, 10, 11], 15.0, 48.0, 12.0).data()
	out['system-folder'] = icons.overlay(base('haiku-system-folder.hvif'),
		[8, 9, 10], 15.0, 48.0, 12.0).data()
	# SoftwareUpdater's package with the mark: System Updater, which updates
	# air/OS itself
	out['system-updater'] = icons.overlay(base('haiku-software-updater.hvif'),
		[], 45.0, 45.0, 15.0).data()
	out['badge'] = icons.badge_icon().data()
	out['partition'] = icons.badge_icon(32.0, 33.0, 24.0).data()
	out['deskbar'] = icons.deskbar_logo().data()
	out['deskbar-dark'] = icons.deskbar_logo(text_rgb=CLOUD_TEXT).data()
	out['logo'] = icons.lockup(A.SLATE)[0].data()
	out['logo-dark'] = icons.lockup(CLOUD_TEXT)[0].data()
	return out


def install_vector_icons(v):
	set_rdef('src/apps/deskbar/icons.rdef', r'resource\(R_LeafLogoBitmap\)', v['deskbar'])
	set_rdef('src/apps/deskbar/icons.rdef', r'resource\(R_LeafLogoDarkBitmap\)', v['deskbar-dark'])
	set_rdef('src/kits/tracker/TrackerIcons.rdef', r'resource\(R_BootVolumeIcon\)', v['boot-volume'])
	set_rdef('src/kits/tracker/TrackerIcons.rdef', r'resource\(R_BeosFolderIcon\)', v['system-folder'])
	set_rdef('src/apps/aboutsystem/AboutSystem.rdef', r'resource vector_icon', v['app'])
	for rel in ('src/apps/aboutsystem/AboutSystem.rdef', 'src/apps/installer/Installer.rdef'):
		set_rdef(rel, r'resource\(1, "airos_logo"\)', v['logo'])
		set_rdef(rel, r'resource\(2, "airos_logo_dark"\)', v['logo-dark'])
	set_rdef('src/apps/haikudepot/HaikuDepot.rdef', r'resource\(560, "native"\)', v['badge'])
	set_rdef('src/apps/systemupdater/SystemUpdater.rdef', r'resource vector_icon',
		v['system-updater'])

	header = path('src/apps/drivesetup/icons.h')
	text = open(header).read()
	start = text.index('const unsigned char kLeaf[]')
	end = text.index('};', start) + 3
	text = text[:start] + rdef.c_array(v['partition'], 'kLeaf') + text[end:]
	open(header, 'w').write(text)
	print('updated src/apps/drivesetup/icons.h')

	for name, data in v.items():
		write('data/artwork/airos/icons/airos-%s.hvif' % name, data)


# -- raster artwork -------------------------------------------------------

def splash_logo():
	"""The primary lockup on the boot screen's black, with room below it
	for the stage icons."""
	width = 200
	surf = lockups.vertical(width, text_rgb=CLOUD_TEXT, pad=0)
	logo = Image.open(io.BytesIO(png_bytes(surf))).convert('RGBA')
	canvas = Image.new('RGBA', (logo.width + 40, logo.height + 34), (0, 0, 0, 255))
	canvas.alpha_composite(logo, (20, 0))
	return canvas.convert('RGB')


def splash_icons(boot_volume_hvif_unused=None):
	"""Haiku's seven stage icons, the boot disk's leaf replaced by the mark.

	The fourth tile shows the boot volume.  Its disk is the Haiku icon at
	32 px, placed at (6, 4) in the 44 x 40 tile (found by matching renders
	against the original); the mark is drawn over the leaf in the colour
	half and a greyed copy, mapped the way the original's grey half is,
	in the lower half.
	"""
	src = Image.open(os.path.join(HERE, 'base', 'haiku-splash-icons.png')).convert('RGBA')
	strip = Image.alpha_composite(Image.new('RGBA', src.size, (0, 0, 0, 255)), src)
	tile_x, size, ox, oy = 132, 32, 6, 4

	mark = icons.add_mark(icons.H.Icon(), 15.0, 48.0, 12.0, gloss=False, simple_below=0.75)
	ic = Image.open(io.BytesIO(png_bytes(hvif_render.render(mark.data(), size)))).convert('RGBA')

	top = strip.crop((tile_x, 0, tile_x + 44, 40)).convert('RGB')
	bottom = strip.crop((tile_x, 40, tile_x + 44, 80)).convert('RGB')
	# Grey half = a * luma(colour half) + b, fitted on the original tile.
	t = np.asarray(top).astype(float)
	b = np.asarray(bottom).astype(float)[..., 0]
	luma = t[..., 0] * 0.299 + t[..., 1] * 0.587 + t[..., 2] * 0.114
	a_fit, b_fit = np.polyfit(luma.ravel(), b.ravel(), 1)

	strip.alpha_composite(ic, (tile_x + ox, oy))
	arr = np.asarray(ic).astype(float)
	gl = arr[..., 0] * 0.299 + arr[..., 1] * 0.587 + arr[..., 2] * 0.114
	g = np.clip(gl * a_fit + b_fit, 0, 255)
	grey = np.dstack([g, g, g, arr[..., 3]]).astype(np.uint8)
	strip.alpha_composite(Image.fromarray(grey, 'RGBA'), (tile_x + ox, 40 + oy))
	return strip.convert('RGB')


def boot_screen(tool):
	logo = splash_logo()
	strip = splash_icons()
	buf = io.BytesIO(); logo.save(buf, 'PNG')
	write('data/artwork/boot_splash/airos_splash_logo.png', buf.getvalue())
	buf = io.BytesIO(); strip.save(buf, 'PNG')
	write('data/artwork/boot_splash/airos_splash_icons.png', buf.getvalue())
	if not tool:
		print('no --boot-screen-tool: images-airos.h not regenerated')
		return
	header = path('headers/private/kernel/boot/images-airos.h')
	subprocess.check_call([tool,
		path('data/artwork/boot_splash/airos_splash_logo.png'), '50', '50',
		path('data/artwork/boot_splash/airos_splash_icons.png'), '50', '50',
		header])
	print('wrote headers/private/kernel/boot/images-airos.h')


def desktop():
	img = wallpaper.render(1920, 1080, None, dither=False)
	tga.write_rle(path('data/artwork/airos/airos-desktop.tga'), img)
	print('wrote data/artwork/airos/airos-desktop.tga')
	preview = img.resize((960, 540), Image.LANCZOS)
	buf = io.BytesIO(); preview.save(buf, 'PNG', optimize=True)
	write('docs/airos/airos-desktop-preview.png', buf.getvalue())


def brand_files():
	write('docs/airos/airos-logo.png', png_bytes(lockups.vertical(560, pad=20)))
	write('data/artwork/airos/airos-logo.png', png_bytes(lockups.vertical(560, pad=20)))
	write('data/artwork/airos/airos-logo-dark.png',
		png_bytes(lockups.vertical(560, text_rgb=CLOUD_TEXT, pad=20)))
	write('data/artwork/airos/airos-lockup.png', png_bytes(lockups.horizontal(128, pad=8)))
	write('data/artwork/airos/airos-mark.svg', mark_svg().encode())
	write('data/artwork/airos/airos-logo.svg', logo_svg().encode())
	# Contact sheet of the icons, for review.
	v = vector_icons()
	sheet = hvif_render.sheet([(k, v[k]) for k in
		('app', 'boot-volume', 'system-folder', 'badge', 'partition')],
		sizes=(16, 32, 64, 128), backgrounds=((216, 216, 216),),
		path=path('docs/airos/airos-icons.png'))
	print('wrote docs/airos/airos-icons.png')


def _svg_color(c):
	return '#%02X%02X%02X' % c


def mark_svg(size=512):
	reg = A.regions()
	r = size / 2.0
	parts = []
	for name, color in (('disc', A.CLOUD), ('sky', A.SKY), ('ring', A.CLOUD),
			('sun', A.DAWN), ('band', A.BREEZE), ('bottom', A.BREEZE)):
		parts.append('<path fill="%s" d="%s"/>' % (_svg_color(color),
			A.svg_path(A.transform(reg[name], r - 1, r, r))))
	return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
		'viewBox="0 0 %d %d">\n%s\n</svg>\n' % (size, size, size, size, '\n'.join(parts)))


def logo_svg(width=560):
	x0, top, x1, bottom = A.wordmark_bounds()
	xh = width / (x1 - x0)
	d = width * 307.0 / 395.3
	r = d / 2.0
	gap = xh * 0.2
	height = d + gap + (bottom - top) * xh
	reg = A.regions()
	parts = []
	for name, color in (('disc', A.CLOUD), ('sky', A.SKY), ('ring', A.CLOUD),
			('sun', A.DAWN), ('band', A.BREEZE), ('bottom', A.BREEZE)):
		parts.append('<path fill="%s" d="%s"/>' % (_svg_color(color),
			A.svg_path(A.transform(reg[name], r - 0.5, width / 2.0, r))))
	baseline = d + gap - top * xh
	word = ' '.join(A.svg_path([[(-x0 * xh + px * xh, baseline + py * xh)
		for px, py in s] for s in p]) for p in A.wordmark())
	parts.append('<path fill="%s" fill-rule="evenodd" d="%s"/>' % (_svg_color(A.SLATE), word))
	return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
		'viewBox="0 0 %d %d">\n%s\n</svg>\n' % (width, round(height), width,
			round(height), '\n'.join(parts)))


def main():
	parser = argparse.ArgumentParser(description=__doc__,
		formatter_class=argparse.RawDescriptionHelpFormatter)
	parser.add_argument('--boot-screen-tool')
	args = parser.parse_args()
	install_vector_icons(vector_icons())
	boot_screen(args.boot_screen_tool)
	desktop()
	brand_files()


if __name__ == '__main__':
	main()
