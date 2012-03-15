#include <linux/export.h>
#include <linux/dma-buf.h>
#include "drmP.h"

struct drm_prime_member {
	struct list_head entry;
	struct dma_buf *dma_buf;
	uint32_t handle;
};

int drm_prime_handle_to_fd_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct drm_prime_handle *args = data;
	struct drm_gem_object *obj;

	if (!drm_core_check_feature(dev, DRIVER_PRIME))
		return -EINVAL;

	if (!dev->driver->prime_export)
		return -ENOSYS;

	obj = drm_gem_object_lookup(dev, file_priv, args->handle);
	if (!obj)
		return -ENOENT;

	if (obj->export_dma_buf) {
		/* TODO what if flags have changed? */
	} else {
		obj->export_dma_buf = dev->driver->prime_export(dev, obj, args->flags);
		if (IS_ERR_OR_NULL(obj->export_dma_buf)) {
			/* normally the created dma-buf takes ownership of the ref,
			 * but if that fails then drop the ref
			 */
			drm_gem_object_unreference_unlocked(obj);
			return PTR_ERR(obj->export_dma_buf);
		}
	}

	args->fd = dma_buf_fd(obj->export_dma_buf);

	return 0;
}

int drm_prime_fd_to_handle_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file_priv)
{
	struct drm_prime_handle *args = data;
	struct dma_buf *dma_buf;
	struct drm_gem_object *obj;
	uint32_t handle;
	int ret;

	if (!drm_core_check_feature(dev, DRIVER_PRIME))
		return -EINVAL;

	if (!dev->driver->prime_import)
		return -ENOSYS;

	dma_buf = dma_buf_get(args->fd);
	if (IS_ERR(dma_buf))
		return PTR_ERR(dma_buf);

	ret = drm_prime_lookup_fd_handle_mapping(&file_priv->prime,
			dma_buf, &handle);
	if (!ret) {
		dma_buf_put(dma_buf);
		args->handle = handle;
		return 0;
	}

	/* never seen this one, need to import */
	obj = dev->driver->prime_import(dev, dma_buf);
	if (IS_ERR_OR_NULL(obj)) {
		ret = PTR_ERR(obj);
		goto fail_put;
	}

	ret = drm_gem_handle_create(file_priv, obj, &handle);
	drm_gem_object_unreference_unlocked(obj);
	if (ret)
		goto fail_put;

	ret = drm_prime_insert_fd_handle_mapping(&file_priv->prime,
			dma_buf, handle);
	if (ret)
		goto fail;

	args->handle = handle;
	return 0;

fail:
	/* hmm, if driver attached, we are relying on the free-object path
	 * to detach.. which seems ok..
	 */
	drm_gem_object_handle_unreference_unlocked(obj);
fail_put:
	dma_buf_put(dma_buf);
	return ret;
}

struct sg_table *drm_prime_pages_to_sg(struct page **pages, int nr_pages)
{
	struct sg_table *sg = NULL;
	struct scatterlist *iter;
	int i;
	int ret;

	sg = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (!sg)
		goto out;

	ret = sg_alloc_table(sg, nr_pages, GFP_KERNEL);
	if (ret)
		goto out;

	for_each_sg(sg->sgl, iter, nr_pages, i)
		sg_set_page(iter, pages[i], PAGE_SIZE, 0);

	return sg;
out:
	kfree(sg);
	return NULL;
}
EXPORT_SYMBOL(drm_prime_pages_to_sg);

/* helper function to cleanup a GEM/prime object */
void drm_prime_gem_destroy(struct drm_gem_object *obj, struct sg_table *sg)
{
	struct dma_buf_attachment *attach;

	attach = obj->import_attach;
	if (sg)
		dma_buf_unmap_attachment(attach, sg, DMA_BIDIRECTIONAL);
	dma_buf_detach(attach->dmabuf, attach);
}
EXPORT_SYMBOL(drm_prime_gem_destroy);

void drm_prime_init_file_private(struct drm_prime_file_private *prime_fpriv)
{
	INIT_LIST_HEAD(&prime_fpriv->head);
}
EXPORT_SYMBOL(drm_prime_init_file_private);

void drm_prime_destroy_file_private(struct drm_prime_file_private *prime_fpriv)
{
	struct drm_prime_member *member, *safe;
	list_for_each_entry_safe(member, safe, &prime_fpriv->head, entry) {
		list_del(&member->entry);
		kfree(member);
	}	
}
EXPORT_SYMBOL(drm_prime_destroy_file_private);

int drm_prime_insert_fd_handle_mapping(struct drm_prime_file_private *prime_fpriv, struct dma_buf *dma_buf, uint32_t handle)
{
	struct drm_prime_member *member;

	member = kmalloc(sizeof(*member), GFP_KERNEL);
	if (!member)
		return -ENOMEM;

	member->dma_buf = dma_buf;
	member->handle = handle;
	list_add(&member->entry, &prime_fpriv->head);
	return 0;
}
EXPORT_SYMBOL(drm_prime_insert_fd_handle_mapping);

int drm_prime_lookup_fd_handle_mapping(struct drm_prime_file_private *prime_fpriv, struct dma_buf *dma_buf, uint32_t *handle)
{
	struct drm_prime_member *member;

	list_for_each_entry(member, &prime_fpriv->head, entry) {
		if (member->dma_buf == dma_buf) {
			*handle = member->handle;
			return 0;
		}
	}
	return -ENOENT;
}
EXPORT_SYMBOL(drm_prime_lookup_fd_handle_mapping);

void drm_prime_remove_fd_handle_mapping(struct drm_prime_file_private *prime_fpriv, struct dma_buf *dma_buf)
{
	struct drm_prime_member *member, *safe;

	list_for_each_entry_safe(member, safe, &prime_fpriv->head, entry) {
		if (member->dma_buf == dma_buf) {
			list_del(&member->entry);
			kfree(member);
		}
	}
}
EXPORT_SYMBOL(drm_prime_remove_fd_handle_mapping);
