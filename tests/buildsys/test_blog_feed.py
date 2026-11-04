"""Syndication must not publish drafts or accidentally opt announcements in."""
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace as NS
import tempfile
import unittest
import xml.etree.ElementTree as ET

from buildsys.blog_feed import CONTENT, DC, on_post_build


def post(slug, draft=False, technical=False):
    return NS(title='SQL & storage', url='blog/' + slug + '/',
              config=NS(draft=draft, date=NS(created=datetime(2026, 11, 4, tzinfo=timezone.utc))),
              meta={'description': 'A <transaction> example', 'author': 'Shujie Zhang',
                    'categories': ['Engineering'] if technical else ['Announcements']},
              content='<p>SQL &amp; storage <a href="../guide/#wal">guide</a></p><img src="../../assets/a.svg" /><pre>SELECT 1 &lt; 2;</pre><section data-blog-lab="weekly" aria-labelledby="report-model"><button>Run model</button><section><p>Nested control</p></section></section><p>After model</p>')


class BlogFeedTests(unittest.TestCase):
    def test_full_text_canonical_urls_author_and_draft_isolation(self):
        with tempfile.TemporaryDirectory() as directory:
            posts = [post('launch'), post('technical', technical=True), post('secret', draft=True, technical=True)]
            config = NS(site_dir=directory, site_url='https://example.org/project/',
                        plugins={'material/blog': NS(blog=NS(posts=posts))})
            on_post_build(config)
            general = ET.parse(Path(directory) / 'blog/feed.xml').findall('./channel/item')
            selected = ET.parse(Path(directory) / 'blog/postgresql.xml').findall('./channel/item')
            self.assertEqual(len(general), 2)
            self.assertEqual(len(selected), 1)
            item = selected[0]
            self.assertEqual(item.findtext('link'), 'https://example.org/project/blog/technical/')
            self.assertEqual(item.findtext('guid'), item.findtext('link'))
            self.assertEqual(item.findtext('{' + DC + '}creator'), 'Shujie Zhang')
            self.assertEqual(item.findtext('title'), 'SQL & storage')
            body = item.findtext('{' + CONTENT + '}encoded')
            self.assertIn('href="https://example.org/project/blog/guide/#wal"', body)
            self.assertIn('src="https://example.org/project/assets/a.svg"', body)
            self.assertIn('SELECT 1 &lt; 2;', body)
            self.assertNotIn('<button', body)
            self.assertNotIn('Nested control', body)
            self.assertIn('https://example.org/project/blog/technical/#report-model', body)
            self.assertIn('After model', body)
            self.assertNotIn('secret', (Path(directory) / 'blog/feed.xml').read_text())
            self.assertEqual(item.findtext('pubDate'), 'Wed, 04 Nov 2026 00:00:00 +0000')

    def test_engineering_category_is_the_only_technical_feed_selector(self):
        with tempfile.TemporaryDirectory() as directory:
            article = post('ordinary')
            config = NS(site_dir=directory, site_url='https://example.org/',
                        plugins={'material/blog': NS(blog=NS(posts=[article]))})
            on_post_build(config)
            path = Path(directory) / 'blog/postgresql.xml'
            self.assertEqual(ET.parse(path).findall('./channel/item'), [])
            article.meta['categories'] = ['Tutorials', 'Engineering']
            on_post_build(config)
            self.assertEqual(len(ET.parse(path).findall('./channel/item')), 1)
            article.config.draft = True
            on_post_build(config)
            self.assertEqual(ET.parse(path).findall('./channel/item'), [])


if __name__ == '__main__':
    unittest.main()
