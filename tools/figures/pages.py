#!/usr/bin/env python3
"""tools/figures/pages.py OUT_DIR: hellion's page and Dawn Patrol as PNGs, from a running stack.

A headless Chromium driven by Playwright renders each page 1280 x 800 CSS pixels at 3
device pixels to one (3840 x 2400), waits until the page has its state (the preview has
a picture, the console is connected), and saves it. No window, no cursor, the same
framing every time. A video preview inside a page keeps its own pixels: the scale
sharpens text and controls.

Needs the stack up (tools/figures/pages.sh brings it up and calls this) and Playwright
with its Chromium (pipx install playwright; playwright install chromium).
HELLION, VEHICLE and TOPSIDE name the three pages.
"""
import json, os, sys, time, urllib.request
from playwright.sync_api import sync_playwright

out = sys.argv[1]
HELLION = os.environ.get("HELLION", "http://127.0.0.1:8092")
VEHICLE = os.environ.get("VEHICLE", "http://127.0.0.1:8080")
TOPSIDE = os.environ.get("TOPSIDE", "http://127.0.0.1:8081")
W, H, SCALE = 1280, 800, 3
os.makedirs(out, exist_ok=True)


def eeye(auto, chain):
    cfg = {"capture_source": "bus", "capture_stream": "camera", "output_device": "",
           "auto_visibility": auto, "chain": chain, "record_path": ""}
    req = urllib.request.Request(HELLION + "/config", json.dumps(cfg).encode(), {"Content-Type": "application/json"})
    urllib.request.urlopen(req, timeout=5).read()


def fresh():
    h = json.load(urllib.request.urlopen(HELLION + "/health", timeout=5))
    age = h["link"]["age_s"]
    if age > 1.0:
        sys.exit("the pilot's picture is %.1f s old: the source has stopped (a file source plays once)" % age)


def shot(page, name):
    path = os.path.join(out, name + ".png")
    page.screenshot(path=path)
    print(name + ".png", "%d x %d" % (W * SCALE, H * SCALE))


with sync_playwright() as p:
    b = p.chromium.launch()
    ctx = b.new_context(viewport={"width": W, "height": H}, device_scale_factor=SCALE)

    # hellion: the page with the preview, the chain as the page shows it
    for name, auto, chain in (("hellion-no-effects", False, []),
                              ("hellion-auto-canny", True, [{"effect": "canny"}])):
        eeye(auto, chain)
        time.sleep(6)  # auto visibility settles with a time constant
        fresh()
        page = ctx.new_page()
        page.goto(HELLION + "/", wait_until="domcontentloaded")
        page.wait_for_function(
            "() => [...document.images].some(i => i.naturalWidth > 300) || "
            "[...document.querySelectorAll('canvas')].some(c => c.width > 300)", timeout=20000)
        time.sleep(3)  # the histogram and the notes are filled from the next status
        shot(page, name)
        page.close()
    # the Fly view's picture: the enhanced one, as a pilot would fly on it
    eeye(True, [])
    time.sleep(6)
    fresh()

    # Dawn Patrol: each view of the vehicle's console, and the topside's Fly view
    for base, who in ((VEHICLE, "vehicle"), (TOPSIDE, "topside")):
        page = ctx.new_page()
        page.goto(base + "/", wait_until="domcontentloaded")
        page.wait_for_function("() => !document.body.innerText.includes('connecting')", timeout=20000)
        time.sleep(2)
        shot(page, "dawn-patrol-%s-setup" % who)
        for tab in ("Topics", "Log", "Hellion"):
            if who != "vehicle":
                break
            try:
                page.get_by_text(tab, exact=True).first.click(timeout=3000)
                time.sleep(2)
                shot(page, "dawn-patrol-%s-%s" % (who, tab.lower()))
            except Exception as e:  # a tab this build does not have
                print("no %s tab: %s" % (tab, str(e).splitlines()[0]))
        page.get_by_text("Fly", exact=True).first.click()
        try:
            page.wait_for_function("() => [...document.images].some(i => i.naturalWidth > 300)", timeout=8000)
        except Exception:
            print("the %s's Fly view has no picture" % who)
        time.sleep(3)
        shot(page, "dawn-patrol-%s-fly" % who)
        page.close()
    eeye(False, [{"effect": "blur"}, {"effect": "sobel"}])
    b.close()
