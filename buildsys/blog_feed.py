"""Build full-text RSS from published Material blog posts, with category feeds."""
from datetime import timezone
from email.utils import format_datetime
from html import escape
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urljoin
import xml.etree.ElementTree as ET

ATOM = 'http://www.w3.org/2005/Atom'
CONTENT = 'http://purl.org/rss/1.0/modules/content/'
DC = 'http://purl.org/dc/elements/1.1/'
ET.register_namespace('atom', ATOM)
ET.register_namespace('content', CONTENT)
ET.register_namespace('dc', DC)


class AbsoluteLinks(HTMLParser):
    """Resolve article links and replace website-only controls with stable links."""
    def __init__(self, base):
        super().__init__(convert_charrefs=False)
        self.base, self.parts, self.hidden_sections = base, [], 0

    def handle_starttag(self, tag, attrs):
        if self.hidden_sections:
            if tag == 'section': self.hidden_sections += 1
            return
        if tag == 'section' and 'data-blog-lab' in dict(attrs):
            # The figure and explanatory tables remain in the feed. Replace
            # website-only controls with a stable link to the interactive model.
            anchor = dict(attrs).get('aria-labelledby', '')
            url = urljoin(self.base, '#' + anchor) if anchor else self.base
            self.parts.append('<p><a href="' + escape(url, quote=True) + '">Open the interactive illustration on the website</a> (no database or test runs in this model).</p>')
            self.hidden_sections = 1
            return
        attrs = [(key, urljoin(self.base, value) if key in {'href', 'src', 'poster'} and value else value)
                 for key, value in attrs]
        self.parts.append('<' + tag + ''.join(' ' + key if value is None else
                          ' ' + key + '="' + escape(value, quote=True) + '"' for key, value in attrs) + '>')

    def handle_startendtag(self, tag, attrs):
        if self.hidden_sections: return
        self.handle_starttag(tag, attrs)
        if self.hidden_sections: self.hidden_sections = 0
        else: self.parts[-1] = self.parts[-1][:-1] + ' />'

    def handle_endtag(self, tag):
        if self.hidden_sections:
            if tag == 'section': self.hidden_sections -= 1
        else: self.parts.append('</' + tag + '>')

    def handle_data(self, data):
        if not self.hidden_sections: self.parts.append(data)

    def handle_entityref(self, name):
        if not self.hidden_sections: self.parts.append('&' + name + ';')

    def handle_charref(self, name):
        if not self.hidden_sections: self.parts.append('&#' + name + ';')

    def handle_comment(self, data):
        if not self.hidden_sections: self.parts.append('<!--' + data + '-->')


def write_feed(path, posts, site_url, title):
    rss = ET.Element('rss', version='2.0')
    channel = ET.SubElement(rss, 'channel')
    for key, value in {'title': title, 'link': urljoin(site_url, 'blog/'),
                       'description': 'PostgreSQL engineering notes by Shujie Zhang.',
                       'language': 'en'}.items():
        ET.SubElement(channel, key).text = value
    feed_url = urljoin(site_url, 'blog/' + path.name)
    ET.SubElement(channel, '{' + ATOM + '}link', href=feed_url, rel='self', type='application/rss+xml')
    posts = sorted(posts, key=lambda post: post.config.date.created, reverse=True)
    if posts:
        ET.SubElement(channel, 'lastBuildDate').text = format_datetime(posts[0].config.date.created.astimezone(timezone.utc))
    for post in posts:
        url = urljoin(site_url, post.url)
        item = ET.SubElement(channel, 'item')
        for key, value in {'title': post.title, 'link': url, 'description': post.meta['description'],
                           'pubDate': format_datetime(post.config.date.created.astimezone(timezone.utc)),
                           '{' + DC + '}creator': post.meta.get('author', 'Shujie Zhang')}.items():
            ET.SubElement(item, key).text = value
        ET.SubElement(item, 'guid', isPermaLink='true').text = url
        for category in post.meta.get('categories', []):
            ET.SubElement(item, 'category').text = category
        body = AbsoluteLinks(url)
        body.feed(post.content)
        body.close()
        ET.SubElement(item, '{' + CONTENT + '}encoded').text = ''.join(body.parts)
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(rss).write(path, encoding='utf-8', xml_declaration=True)


def on_post_build(config):
    # Reuse the blog plugin's resolved URLs, rendered HTML and publication dates.
    # Preview-only drafts must never leak into an aggregator feed.
    posts = [post for post in config.plugins['material/blog'].blog.posts if not post.config.draft]
    technical = [post for post in posts if 'Engineering' in post.meta.get('categories', [])]
    root = Path(config.site_dir) / 'blog'
    write_feed(root / 'feed.xml', posts, config.site_url, 'PostGamma — Shujie Zhang')
    write_feed(root / 'postgresql.xml', technical, config.site_url, 'Shujie Zhang — PostgreSQL engineering')
