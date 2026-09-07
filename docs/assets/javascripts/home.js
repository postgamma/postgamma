(function () {
  "use strict";

  var MORPH_SOURCE = "PostgreSQL";
  var MORPH_STEM = "Postg";
  var MORPH_TARGET = "postgamma";
  var MORPH_INITIAL_DELAY = 1800;
  var MORPH_DELETING_DELAY = 150;
  var MORPH_STEM_DELAY = 280;
  var MORPH_CASE_SHIFT_DELAY = 320;
  var MORPH_CASE_HOLD_DELAY = 180;
  var MORPH_TYPING_DELAY = 160;
  var GITHUB_STAR_CACHE_KEY = "postgamma.github.star-count";
  var GITHUB_STAR_CACHE_TTL = 15 * 60 * 1000;

  function wait(milliseconds) {
    return new Promise(function (resolve) {
      window.setTimeout(resolve, milliseconds);
    });
  }

  function shouldReduceMotion() {
    return window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  }

  function canContinueMorph(element) {
    if (!element.isConnected) {
      return false;
    }

    if (shouldReduceMotion()) {
      element.textContent = MORPH_TARGET;
      element.setAttribute("data-pg-morph-state", "done");
      return false;
    }

    return true;
  }

  function animateCaseShift(element) {
    return new Promise(function (resolve) {
      var letter = document.createElement("span");

      letter.className = "pg-v3-morph-letter";
      letter.textContent = MORPH_STEM.charAt(0);
      element.replaceChildren(letter, document.createTextNode(MORPH_STEM.slice(1)));

      window.requestAnimationFrame(function () {
        letter.classList.add("is-shifting");

        window.setTimeout(function () {
          letter.textContent = MORPH_STEM.charAt(0).toLowerCase();
        }, MORPH_CASE_SHIFT_DELAY / 2);

        window.setTimeout(function () {
          element.textContent = MORPH_STEM.toLowerCase();
          resolve();
        }, MORPH_CASE_SHIFT_DELAY + 10);
      });
    });
  }

  async function runWordMorph(element) {
    var current = MORPH_SOURCE;
    var suffix = MORPH_TARGET.slice(MORPH_STEM.length);
    var index;

    if (element.getAttribute("data-pg-morph-started") === "true") {
      return;
    }

    element.setAttribute("data-pg-morph-started", "true");

    if (shouldReduceMotion()) {
      element.textContent = MORPH_TARGET;
      element.setAttribute("data-pg-morph-state", "done");
      return;
    }

    element.textContent = MORPH_SOURCE;
    element.setAttribute("data-pg-morph-state", "running");
    await wait(MORPH_INITIAL_DELAY);

    while (current.length > MORPH_STEM.length) {
      if (!canContinueMorph(element)) {
        return;
      }

      current = current.slice(0, -1);
      element.textContent = current;
      await wait(MORPH_DELETING_DELAY);
    }

    await wait(MORPH_STEM_DELAY);

    if (!canContinueMorph(element)) {
      return;
    }

    await animateCaseShift(element);
    current = MORPH_STEM.toLowerCase();
    await wait(MORPH_CASE_HOLD_DELAY);

    for (index = 0; index < suffix.length; index += 1) {
      if (!canContinueMorph(element)) {
        return;
      }

      current += suffix[index];
      element.textContent = current;
      await wait(MORPH_TYPING_DELAY);
    }

    element.textContent = MORPH_TARGET;
    element.setAttribute("data-pg-morph-state", "done");
  }

  function initializeWordMorph() {
    var element = document.querySelector("[data-pg-word-morph]");
    var fontsReady;
    var start;

    if (!element) {
      return;
    }

    start = function () {
      if (element.isConnected) {
        runWordMorph(element);
      }
    };

    if (shouldReduceMotion()) {
      start();
      return;
    }

    fontsReady = document.fonts && document.fonts.ready
      ? document.fonts.ready
      : Promise.resolve();

    fontsReady.then(start, start);
  }

  function formatStarCount(count) {
    if (count < 1000) {
      return count.toLocaleString("en-US");
    }

    try {
      return new Intl.NumberFormat("en", {
        notation: "compact",
        maximumFractionDigits: 1
      }).format(count);
    } catch (error) {
      return Math.round(count / 100) / 10 + "k";
    }
  }

  function renderGitHubStarCount(link, count) {
    var value = link.querySelector("[data-pg-star-count]");

    if (!value) {
      return;
    }

    value.textContent = formatStarCount(count);
    link.setAttribute(
      "aria-label",
      "postgamma on GitHub, " + count.toLocaleString("en-US") + " stars"
    );
  }

  function readCachedGitHubStarCount() {
    var cached;

    try {
      cached = JSON.parse(window.localStorage.getItem(GITHUB_STAR_CACHE_KEY));
    } catch (error) {
      return null;
    }

    if (
      !cached ||
      !Number.isInteger(cached.count) ||
      cached.count < 0 ||
      !Number.isFinite(cached.savedAt) ||
      Date.now() - cached.savedAt >= GITHUB_STAR_CACHE_TTL
    ) {
      return null;
    }

    return cached.count;
  }

  function cacheGitHubStarCount(count) {
    try {
      window.localStorage.setItem(
        GITHUB_STAR_CACHE_KEY,
        JSON.stringify({ count: count, savedAt: Date.now() })
      );
    } catch (error) {
      // Storage can be unavailable in private browsing modes.
    }
  }

  function initializeGitHubStarCount() {
    var link = document.querySelector("[data-pg-github-stars]");
    var cached;
    var repositoryApi;

    if (!link || typeof window.fetch !== "function") {
      return;
    }

    cached = readCachedGitHubStarCount();
    if (cached !== null) {
      renderGitHubStarCount(link, cached);
      return;
    }

    repositoryApi = link.getAttribute("data-pg-repository-api");
    window.fetch(repositoryApi, {
      headers: { Accept: "application/vnd.github+json" }
    }).then(function (response) {
      if (!response.ok) {
        throw new Error("GitHub repository request failed");
      }
      return response.json();
    }).then(function (repository) {
      var count = repository.stargazers_count;

      if (!Number.isInteger(count) || count < 0) {
        return;
      }

      renderGitHubStarCount(link, count);
      cacheGitHubStarCount(count);
    }).catch(function () {
      // Keep the build-time count when GitHub is temporarily unavailable.
    });
  }

  function syncHomepageState() {
    var homepage = document.querySelector(".pg-home-v3");
    var themeToggle = document.querySelector("[data-pg-theme-toggle]");
    var isDark = document.body.getAttribute("data-md-color-scheme") === "slate";

    document.body.classList.toggle("pg-home-page", Boolean(homepage));

    if (themeToggle) {
      themeToggle.setAttribute(
        "aria-label",
        isDark ? "Switch to light mode" : "Switch to dark mode"
      );
      themeToggle.setAttribute(
        "title",
        isDark ? "Switch to light mode" : "Switch to dark mode"
      );
    }
  }

  function initializeHomepage() {
    syncHomepageState();
    initializeWordMorph();
    initializeGitHubStarCount();
  }

  function activateTab(tab, moveFocus) {
    var tabList = tab.closest("[data-pg-code-tabs]");
    var terminal = tab.closest("[data-pg-code-switcher]");

    if (!tabList || !terminal) {
      return;
    }

    tabList.querySelectorAll("[role='tab']").forEach(function (candidate) {
      var selected = candidate === tab;
      var panelId = candidate.getAttribute("aria-controls");
      var panel = document.getElementById(panelId);

      candidate.setAttribute("aria-selected", selected ? "true" : "false");
      candidate.tabIndex = selected ? 0 : -1;
      if (panel) {
        panel.hidden = !selected;
      }
    });

    if (moveFocus) {
      tab.focus();
    }
  }

  document.addEventListener("click", function (event) {
    var tab = event.target.closest("[data-pg-code-tab]");
    var themeToggle = event.target.closest("[data-pg-theme-toggle]");

    if (tab) {
      activateTab(tab, false);
    }

    if (themeToggle) {
      var isDark = document.body.getAttribute("data-md-color-scheme") === "slate";
      var targetPalette = isDark ? "__palette_1" : "__palette_0";
      var paletteLabel = document.querySelector("label[for='" + targetPalette + "']");

      if (paletteLabel) {
        paletteLabel.click();
        window.requestAnimationFrame(syncHomepageState);
      }
    }

  });

  document.addEventListener("keydown", function (event) {
    var tab = event.target.closest("[data-pg-code-tab]");
    var tabs;
    var currentIndex;
    var nextIndex;

    if (!tab || !["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) {
      return;
    }

    tabs = Array.from(tab.parentElement.querySelectorAll("[role='tab']"));
    currentIndex = tabs.indexOf(tab);

    if (event.key === "Home") {
      nextIndex = 0;
    } else if (event.key === "End") {
      nextIndex = tabs.length - 1;
    } else {
      nextIndex = (currentIndex + (event.key === "ArrowRight" ? 1 : -1) + tabs.length) % tabs.length;
    }

    event.preventDefault();
    activateTab(tabs[nextIndex], true);
  });

  new MutationObserver(syncHomepageState).observe(document.body, {
    attributes: true,
    attributeFilter: ["data-md-color-scheme"]
  });

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initializeHomepage);
  } else {
    initializeHomepage();
  }
}());
