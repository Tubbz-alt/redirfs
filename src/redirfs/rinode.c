#include "redir.h"

static struct kmem_cache *rinode_cache = NULL;
unsigned long long rinode_cnt = 0;
spinlock_t rinode_cnt_lock = SPIN_LOCK_UNLOCKED;
extern atomic_t rinodes_freed;
extern wait_queue_head_t rinodes_wait;

struct rinode *rinode_alloc(struct inode *inode)
{
	struct rinode *rinode = NULL;
	unsigned long flags;


	rinode = kmem_cache_alloc(rinode_cache, GFP_KERNEL);
	if (!rinode)
		return ERR_PTR(-ENOMEM);

	INIT_RCU_HEAD(&rinode->ri_rcu);
	INIT_LIST_HEAD(&rinode->ri_rdentries);
	INIT_LIST_HEAD(&rinode->ri_data);
	rinode->ri_inode = inode;
	rinode->ri_op_old = (struct inode_operations *)inode->i_op;
	rinode->ri_fop_old = (struct file_operations *)inode->i_fop;
	rinode->ri_aop_old = (struct address_space_operations *)inode->i_mapping->a_ops;
	rinode->ri_path = NULL;
	rinode->ri_chain = NULL;
	rinode->ri_path_set = NULL;
	rinode->ri_chain_set = NULL;
	rinode->ri_ops_set = NULL;
	atomic_set(&rinode->ri_count, 1);
	atomic_set(&rinode->ri_nlink, 1);
	spin_lock_init(&rinode->ri_lock);

	if (inode->i_op)
		memcpy(&rinode->ri_op_new, inode->i_op,
				sizeof(struct inode_operations));
	else
		memset(&rinode->ri_op_new, 0,
				sizeof(struct inode_operations));

	if (inode->i_mapping->a_ops)
		memcpy(&rinode->ri_aop_new, inode->i_mapping->a_ops,
				sizeof(struct address_space_operations));
	else
		memset(&rinode->ri_aop_new, 0,
				sizeof(struct address_space_operations));

	rinode->ri_op_new.lookup = rfs_lookup;

	spin_lock_irqsave(&rinode_cnt_lock, flags);
	rinode_cnt++;
	spin_unlock_irqrestore(&rinode_cnt_lock, flags);

	return rinode;
}

inline struct rinode *rinode_get(struct rinode *rinode)
{
	BUG_ON(!atomic_read(&rinode->ri_count));
	atomic_inc(&rinode->ri_count);
	return rinode;
}

inline void rinode_put(struct rinode *rinode)
{
	unsigned long flags;
	struct rfs_priv_data *data;
	struct rfs_priv_data *tmp;

	if (!rinode || IS_ERR(rinode))
		return;

	BUG_ON(!atomic_read(&rinode->ri_count));
	if (!atomic_dec_and_test(&rinode->ri_count))
		return;
	
	path_put(rinode->ri_path);
	chain_put(rinode->ri_chain);
	path_put(rinode->ri_path_set);
	chain_put(rinode->ri_chain_set);
	ops_put(rinode->ri_ops_set);
	BUG_ON(!list_empty(&rinode->ri_rdentries));

	list_for_each_entry_safe(data, tmp, &rinode->ri_data, list) {
		spin_lock_irqsave(&rinode->ri_lock, flags);
		list_del(&data->list);
		spin_unlock_irqrestore(&rinode->ri_lock, flags);
		rfs_put_data(data);
	}

	kmem_cache_free(rinode_cache, rinode);

	spin_lock_irqsave(&rinode_cnt_lock, flags);
	if (!--rinode_cnt)
		atomic_set(&rinodes_freed, 1);
	spin_unlock_irqrestore(&rinode_cnt_lock, flags);

	if (atomic_read(&rinodes_freed))
		wake_up_interruptible(&rinodes_wait);
}

inline struct rinode *rinode_find(struct inode *inode)
{
	struct rinode *rinode = NULL;
	const struct inode_operations *i_op;


	rcu_read_lock();
	i_op = rcu_dereference(inode->i_op);
	if (i_op) {
		if (i_op->lookup == rfs_lookup) {
			rinode = container_of(i_op, struct rinode, ri_op_new);
			rinode = rinode_get(rinode);
		}
	}
	rcu_read_unlock();

	return rinode;
}

static inline void rinode_del_rcu(struct rcu_head *head)
{
	struct rinode *rinode = NULL;

	rinode = container_of(head, struct rinode, ri_rcu);
	rinode_put(rinode);
}

void rinode_del(struct inode *inode)
{
	struct rinode *rinode = NULL;

	spin_lock(&inode->i_lock);

	rinode = rinode_find(inode);
	if (!rinode) {
		spin_unlock(&inode->i_lock);
		return;
	}

	if (!atomic_dec_and_test(&rinode->ri_nlink)) {
		spin_unlock(&inode->i_lock);
		rinode_put(rinode);
		return;
	}

	if (!S_ISSOCK(inode->i_mode))
		inode->i_fop = rinode->ri_fop_old;

	if (S_ISREG(inode->i_mode))
		inode->i_mapping->a_ops = rinode->ri_aop_old;

	rcu_assign_pointer(inode->i_op, rinode->ri_op_old);

	rinode_put(rinode);

	spin_unlock(&inode->i_lock);

	call_rcu(&rinode->ri_rcu, rinode_del_rcu);
}

int rfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct rinode *rinode = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;

	rinode = rinode_find(dir);
	if (!rinode) {
		if (dir->i_op && dir->i_op->unlink)
			 return dir->i_op->unlink(dir, dentry);

		return -EPERM;
	}

	spin_lock(&rinode->ri_lock);
	chain = chain_get(rinode->ri_chain);
	spin_unlock(&rinode->ri_lock);

	args.args.i_unlink.dir = dir;
	args.args.i_unlink.dentry = dentry;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISDIR(dir->i_mode))
		args.type.id = RFS_DIR_IOP_UNLINK;
	else
		BUG();

	if (!rfs_precall_flts(0, chain, &cont, &args)) {
		if (rinode->ri_op_old && rinode->ri_op_old->unlink)
			rv = rinode->ri_op_old->unlink(args.args.i_unlink.dir, args.args.i_unlink.dentry);
		else
			rv = -EPERM;

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);
	rv = args.retv.rv_int;

	chain_put(chain);
	rinode_put(rinode);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct rinode *rinode = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;

	rinode = rinode_find(dir);
	if (!rinode) {
		if (dir->i_op && dir->i_op->rmdir)
			 return dir->i_op->rmdir(dir, dentry);

		return -EPERM;
	}

	spin_lock(&rinode->ri_lock);
	chain = chain_get(rinode->ri_chain);
	spin_unlock(&rinode->ri_lock);

	args.args.i_rmdir.dir = dir;
	args.args.i_rmdir.dentry = dentry;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISDIR(dir->i_mode))
		args.type.id = RFS_DIR_IOP_RMDIR;
	else
		BUG();

	if (!rfs_precall_flts(0, chain, &cont, &args)) {
		if (rinode->ri_op_old && rinode->ri_op_old->rmdir)
			rv = rinode->ri_op_old->rmdir(args.args.i_rmdir.dir, args.args.i_rmdir.dentry);
		else
			rv = -EPERM;

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);
	rv = args.retv.rv_int;

	chain_put(chain);
	rinode_put(rinode);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct rinode *parent = NULL;
	struct rpath *path = NULL;
	struct rdentry *rdentry = NULL;
	struct rinode *rinode = NULL;
	struct rpath *path_set = NULL;
	struct chain *chain_set = NULL;
	struct ops *ops_set = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;

	parent = rinode_find(dir);
	if (!parent) {
		if (dir->i_op && dir->i_op->mkdir)
			 return dir->i_op->mkdir(dir, dentry, mode);

		return -EPERM;
	}

	spin_lock(&parent->ri_lock);
	path_set = path_get(parent->ri_path_set);
	chain_set = chain_get(parent->ri_chain_set);
	ops_set = ops_get(parent->ri_ops_set);
	path = path_get(parent->ri_path);
	chain = chain_get(parent->ri_chain);
	spin_unlock(&parent->ri_lock);

	args.args.i_mkdir.dir = dir;
	args.args.i_mkdir.dentry = dentry;
	args.args.i_mkdir.mode = mode;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISDIR(dir->i_mode))
		args.type.id = RFS_DIR_IOP_MKDIR;
	else
		BUG();

	if (!rfs_precall_flts(0, chain, &cont, &args)) {

		if (parent->ri_op_old && parent->ri_op_old->mkdir)
			rv = parent->ri_op_old->mkdir(args.args.i_mkdir.dir, args.args.i_mkdir.dentry, args.args.i_mkdir.mode);
		else
			rv = -EPERM;

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);
	
	rv = args.retv.rv_int;

	if (!chain_set)
		goto exit;

	rdentry = rdentry_add(dentry);
	if (IS_ERR(rdentry)) {
		BUG();
		goto exit;
	}

	spin_lock(&rdentry->rd_lock);
	path_put(rdentry->rd_path);
	chain_put(rdentry->rd_chain);
	ops_put(rdentry->rd_ops);
	rdentry->rd_path = path_get(path_set);
	rdentry->rd_chain = chain_get(chain_set);
	rdentry->rd_ops = ops_get(ops_set);
	spin_unlock(&rdentry->rd_lock);
	rdentry_set_ops(rdentry, ops_set);

	rinode = rdentry->rd_rinode;
	if (rinode) {
		spin_lock(&rinode->ri_lock);
		path_put(rinode->ri_path_set);
		chain_put(rinode->ri_chain_set);
		ops_put(rinode->ri_ops_set);
		path_put(rinode->ri_path);
		chain_put(rinode->ri_chain);
		rinode->ri_path_set = path_get(path_set);
		rinode->ri_chain_set = chain_get(chain_set);
		rinode->ri_ops_set = ops_get(ops_set);
		rinode->ri_path = path_get(path_set);
		rinode->ri_chain = chain_get(chain_set);
		spin_unlock(&rinode->ri_lock);
		rinode_set_ops(rinode, ops_set);
	}

exit:
	rdentry_put(rdentry);
	rinode_put(parent);
	path_put(path_set);
	chain_put(chain_set);
	ops_put(ops_set);
	path_put(path);
	chain_put(chain);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
	struct rinode *parent = NULL;
	struct rpath *path = NULL;
	struct rdentry *rdentry = NULL;
	struct rinode *rinode = NULL;
	struct rpath *path_set = NULL;
	struct chain *chain_set = NULL;
	struct ops *ops_set = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;

	parent = rinode_find(dir);
	if (!parent) {
		if (dir->i_op && dir->i_op->create)
			return dir->i_op->create(dir, dentry, mode, nd);

		return -EACCES;
	}

	spin_lock(&parent->ri_lock);
	path_set = path_get(parent->ri_path_set);
	chain_set = chain_get(parent->ri_chain_set);
	ops_set = ops_get(parent->ri_ops_set);
	path = path_get(parent->ri_path);
	chain = chain_get(parent->ri_chain);
	spin_unlock(&parent->ri_lock);

	args.args.i_create.dir = dir;
	args.args.i_create.dentry = dentry;
	args.args.i_create.mode = mode;
	args.args.i_create.nd = nd;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISDIR(dir->i_mode))
		args.type.id = RFS_DIR_IOP_CREATE;
	else
		BUG();

	if (!rfs_precall_flts(0, chain, &cont, &args)) {

		if (parent->ri_op_old && parent->ri_op_old->create)
			rv = parent->ri_op_old->create(args.args.i_create.dir, args.args.i_create.dentry, args.args.i_create.mode, args.args.i_create.nd);
		else
			rv = -EACCES;

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);

	rv = args.retv.rv_int;

	if (!chain_set)
		goto exit;

	rdentry = rdentry_add(dentry);
	if (IS_ERR(rdentry)) {
		BUG();
		goto exit;
	}

	spin_lock(&rdentry->rd_lock);
	path_put(rdentry->rd_path);
	chain_put(rdentry->rd_chain);
	ops_put(rdentry->rd_ops);
	rdentry->rd_path = path_get(path_set);
	rdentry->rd_chain = chain_get(chain_set);
	rdentry->rd_ops = ops_get(ops_set);
	spin_unlock(&rdentry->rd_lock);
	rdentry_set_ops(rdentry, ops_set);

	rinode = rdentry->rd_rinode;
	if (rinode) {
		spin_lock(&rinode->ri_lock);
		path_put(rinode->ri_path_set);
		chain_put(rinode->ri_chain_set);
		ops_put(rinode->ri_ops_set);
		path_put(rinode->ri_path);
		chain_put(rinode->ri_chain);
		rinode->ri_path_set = path_get(path_set);
		rinode->ri_chain_set = chain_get(chain_set);
		rinode->ri_ops_set = ops_get(ops_set);
		rinode->ri_path = path_get(path_set);
		rinode->ri_chain = chain_get(chain_set);
		spin_unlock(&rinode->ri_lock);
		rinode_set_ops(rinode, ops_set);
	}

exit:
	rdentry_put(rdentry);
	rinode_put(parent);
	path_put(path_set);
	chain_put(chain_set);
	ops_put(ops_set);
	path_put(path);
	chain_put(chain);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_link(struct dentry *old_dentry,
		struct inode *dir, struct dentry *dentry)
{
	struct rinode *parent = NULL;
	struct rpath *path = NULL;
	struct rdentry *rdentry = NULL;
	struct rinode *rinode = NULL;
	struct rpath *path_set = NULL;
	struct chain *chain_set = NULL;
	struct ops *ops_set = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;

	parent = rinode_find(dir);
	if (!parent) {
		if (dir->i_op && dir->i_op->link)
			return dir->i_op->link(old_dentry, dir, dentry);

		return -EPERM;
	}

	spin_lock(&parent->ri_lock);
	path_set = path_get(parent->ri_path_set);
	chain_set = chain_get(parent->ri_chain_set);
	ops_set = ops_get(parent->ri_ops_set);
	path = path_get(parent->ri_path);
	chain = chain_get(parent->ri_chain);
	spin_unlock(&parent->ri_lock);

	args.args.i_link.old_dentry = old_dentry;
	args.args.i_link.dir = dir;
	args.args.i_link.dentry = dentry;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISDIR(dir->i_mode))
		args.type.id = RFS_DIR_IOP_LINK;
	else
		BUG();

	if (!rfs_precall_flts(0, chain, &cont, &args)) {

		if (parent->ri_op_old && parent->ri_op_old->link)
			rv = parent->ri_op_old->link(args.args.i_link.old_dentry, args.args.i_link.dir, args.args.i_link.dentry);
		else
			rv = -EPERM;

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);

	rv = args.retv.rv_int;

	if (!chain_set)
		goto exit;

	rdentry = rdentry_add(dentry);
	if (IS_ERR(rdentry)) {
		BUG();
		goto exit;
	}

	spin_lock(&rdentry->rd_lock);
	path_put(rdentry->rd_path);
	chain_put(rdentry->rd_chain);
	ops_put(rdentry->rd_ops);
	rdentry->rd_path = path_get(path_set);
	rdentry->rd_chain = chain_get(chain_set);
	rdentry->rd_ops = ops_get(ops_set);
	spin_unlock(&rdentry->rd_lock);
	rdentry_set_ops(rdentry, ops_set);

	rinode = rdentry->rd_rinode;
	if (rinode) {
		spin_lock(&rinode->ri_lock);
		path_put(rinode->ri_path_set);
		chain_put(rinode->ri_chain_set);
		ops_put(rinode->ri_ops_set);
		path_put(rinode->ri_path);
		chain_put(rinode->ri_chain);
		rinode->ri_path_set = path_get(path_set);
		rinode->ri_chain_set = chain_get(chain_set);
		rinode->ri_ops_set = ops_get(ops_set);
		rinode->ri_path = path_get(path_set);
		rinode->ri_chain = chain_get(chain_set);
		spin_unlock(&rinode->ri_lock);
		rinode_set_ops(rinode, ops_set);
	}

exit:
	rdentry_put(rdentry);
	rinode_put(parent);
	path_put(path_set);
	chain_put(chain_set);
	ops_put(ops_set);
	path_put(path);
	chain_put(chain);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname)
{
	struct rinode *parent = NULL;
	struct rpath *path = NULL;
	struct rdentry *rdentry = NULL;
	struct rinode *rinode = NULL;
	struct rpath *path_set = NULL;
	struct chain *chain_set = NULL;
	struct ops *ops_set = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;

	parent = rinode_find(dir);
	if (!parent) {
		if (dir->i_op && dir->i_op->symlink)
			return dir->i_op->symlink(dir, dentry, oldname);

		return -EPERM;
	}

	spin_lock(&parent->ri_lock);
	path_set = path_get(parent->ri_path_set);
	chain_set = chain_get(parent->ri_chain_set);
	ops_set = ops_get(parent->ri_ops_set);
	path = path_get(parent->ri_path);
	chain = chain_get(parent->ri_chain);
	spin_unlock(&parent->ri_lock);

	args.args.i_symlink.dir = dir;
	args.args.i_symlink.dentry = dentry;
	args.args.i_symlink.oldname = oldname;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISDIR(dir->i_mode))
		args.type.id = RFS_DIR_IOP_SYMLINK;
	else
		BUG();

	if (!rfs_precall_flts(0, chain, &cont, &args)) {

		if (parent->ri_op_old && parent->ri_op_old->link)
			rv = parent->ri_op_old->symlink(args.args.i_symlink.dir, args.args.i_symlink.dentry, args.args.i_symlink.oldname);
		else
			rv = -EPERM;

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);

	rv = args.retv.rv_int;

	if (!chain_set)
		goto exit;

	rdentry = rdentry_add(dentry);
	if (IS_ERR(rdentry)) {
		BUG();
		goto exit;
	}

	spin_lock(&rdentry->rd_lock);
	path_put(rdentry->rd_path);
	chain_put(rdentry->rd_chain);
	ops_put(rdentry->rd_ops);
	rdentry->rd_path = path_get(path_set);
	rdentry->rd_chain = chain_get(chain_set);
	rdentry->rd_ops = ops_get(ops_set);
	spin_unlock(&rdentry->rd_lock);
	rdentry_set_ops(rdentry, ops_set);

	rinode = rdentry->rd_rinode;
	if (rinode) {
		spin_lock(&rinode->ri_lock);
		path_put(rinode->ri_path_set);
		chain_put(rinode->ri_chain_set);
		ops_put(rinode->ri_ops_set);
		path_put(rinode->ri_path);
		chain_put(rinode->ri_chain);
		rinode->ri_path_set = path_get(path_set);
		rinode->ri_chain_set = chain_get(chain_set);
		rinode->ri_ops_set = ops_get(ops_set);
		rinode->ri_path = path_get(path_set);
		rinode->ri_chain = chain_get(chain_set);
		spin_unlock(&rinode->ri_lock);
		rinode_set_ops(rinode, ops_set);
	}

exit:
	rdentry_put(rdentry);
	rinode_put(parent);
	path_put(path_set);
	chain_put(chain_set);
	ops_put(ops_set);
	path_put(path);
	chain_put(chain);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

struct dentry *rfs_lookup(struct inode *dir, struct dentry *dentry, struct nameidata *nd)
{
	struct rinode *parent = NULL;
	struct rpath *path = NULL;
	struct rdentry *rdentry = NULL;
	struct rinode *rinode = NULL;
	struct rpath *path_set = NULL;
	struct chain *chain_set = NULL;
	struct ops *ops_set = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct dentry *rv = NULL;
	struct context cont;

	parent = rinode_find(dir);
	if (!parent) {
		if (dir->i_op && dir->i_op->lookup)
			return dir->i_op->lookup(dir, dentry, nd);

		return ERR_PTR(-ENOSYS);
	}

	spin_lock(&parent->ri_lock);
	path_set = path_get(parent->ri_path_set);
	chain_set = chain_get(parent->ri_chain_set);
	ops_set = ops_get(parent->ri_ops_set);
	path = path_get(parent->ri_path);
	chain = chain_get(parent->ri_chain);
	spin_unlock(&parent->ri_lock);

	args.args.i_lookup.dir = dir;
	args.args.i_lookup.dentry = dentry;
	args.args.i_lookup.nd = nd;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISDIR(dir->i_mode))
		args.type.id = RFS_DIR_IOP_LOOKUP;
	else
		BUG();

	if (!rfs_precall_flts(0, chain, &cont, &args)) {

		if (parent->ri_op_old && parent->ri_op_old->lookup)
			rv = parent->ri_op_old->lookup(args.args.i_lookup.dir, args.args.i_lookup.dentry, args.args.i_lookup.nd);
		else
			rv = ERR_PTR(-ENOSYS);

		args.retv.rv_dentry = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);

	rv = args.retv.rv_dentry;

	if (!chain_set)
		goto exit;

	rdentry = rdentry_add(dentry);
	if (IS_ERR(rdentry)) {
		BUG();
		goto exit;
	}

	spin_lock(&rdentry->rd_lock);
	path_put(rdentry->rd_path);
	chain_put(rdentry->rd_chain);
	ops_put(rdentry->rd_ops);
	rdentry->rd_path = path_get(path_set);
	rdentry->rd_chain = chain_get(chain_set);
	rdentry->rd_ops = ops_get(ops_set);
	spin_unlock(&rdentry->rd_lock);
	rdentry_set_ops(rdentry, ops_set);

	rinode = rdentry->rd_rinode;
	if (rinode) {
		spin_lock(&rinode->ri_lock);
		path_put(rinode->ri_path_set);
		chain_put(rinode->ri_chain_set);
		ops_put(rinode->ri_ops_set);
		path_put(rinode->ri_path);
		chain_put(rinode->ri_chain);
		rinode->ri_path_set = path_get(path_set);
		rinode->ri_chain_set = chain_get(chain_set);
		rinode->ri_ops_set = ops_get(ops_set);
		rinode->ri_path = path_get(path_set);
		rinode->ri_chain = chain_get(chain_set);
		spin_unlock(&rinode->ri_lock);
		rinode_set_ops(rinode, ops_set);
	}

exit:
	rdentry_put(rdentry);
	rinode_put(parent);
	path_put(path_set);
	chain_put(chain_set);
	ops_put(ops_set);
	path_put(path);
	chain_put(chain);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_mknod(struct inode * dir, struct dentry *dentry, int mode, dev_t rdev)
{
	struct rinode *parent = NULL;
	struct rpath *path = NULL;
	struct rdentry *rdentry = NULL;
	struct rinode *rinode = NULL;
	struct rpath *path_set = NULL;
	struct chain *chain_set = NULL;
	struct ops *ops_set = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;

	parent = rinode_find(dir);
	if (!parent) {
		if (dir->i_op && dir->i_op->mknod)
			return dir->i_op->mknod(dir, dentry, mode, rdev);
		
		return -EPERM;
	}

	spin_lock(&parent->ri_lock);
	path_set = path_get(parent->ri_path_set);
	chain_set = chain_get(parent->ri_chain_set);
	ops_set = ops_get(parent->ri_ops_set);
	path = path_get(parent->ri_path);
	chain = chain_get(parent->ri_chain);
	spin_unlock(&parent->ri_lock);

	args.args.i_mknod.dir = dir;
	args.args.i_mknod.dentry = dentry;
	args.args.i_mknod.mode = mode;
	args.args.i_mknod.rdev = rdev;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISDIR(dir->i_mode))
		args.type.id = RFS_DIR_IOP_MKNOD;
	else
		BUG();

	if (!rfs_precall_flts(0, chain, &cont, &args)) {

		if (parent->ri_op_old && parent->ri_op_old->mknod)
			rv = parent->ri_op_old->mknod(args.args.i_mknod.dir, args.args.i_mknod.dentry, args.args.i_mknod.mode, args.args.i_mknod.rdev);
		else
			rv = -EPERM;

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);

	rv = args.retv.rv_int;

	if (!chain_set)
		goto exit;

	rdentry = rdentry_add(dentry);
	if (IS_ERR(rdentry)) {
		BUG();
		goto exit;
	}

	spin_lock(&rdentry->rd_lock);
	path_put(rdentry->rd_path);
	chain_put(rdentry->rd_chain);
	ops_put(rdentry->rd_ops);
	rdentry->rd_path = path_get(path_set);
	rdentry->rd_chain = chain_get(chain_set);
	rdentry->rd_ops = ops_get(ops_set);
	spin_unlock(&rdentry->rd_lock);
	rdentry_set_ops(rdentry, ops_set);

	rinode = rdentry->rd_rinode;
	if (rinode) {
		spin_lock(&rinode->ri_lock);
		path_put(rinode->ri_path_set);
		chain_put(rinode->ri_chain_set);
		ops_put(rinode->ri_ops_set);
		path_put(rinode->ri_path);
		chain_put(rinode->ri_chain);
		rinode->ri_path_set = path_get(path_set);
		rinode->ri_chain_set = chain_get(chain_set);
		rinode->ri_ops_set = ops_get(ops_set);
		rinode->ri_path = path_get(path_set);
		rinode->ri_chain = chain_get(chain_set);
		spin_unlock(&rinode->ri_lock);
		rinode_set_ops(rinode, ops_set);
	}

exit:
	rdentry_put(rdentry);
	rinode_put(parent);
	path_put(path_set);
	chain_put(chain_set);
	ops_put(ops_set);
	path_put(path);
	chain_put(chain);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_permission(struct inode *inode, int mask, struct nameidata *nd)
{
	struct rinode *rinode = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int submask = mask & ~MAY_APPEND;
	int rv = 0;

	rinode = rinode_find(inode);
	if (!rinode) {
		if (inode->i_op && inode->i_op->permission)
			return inode->i_op->permission(inode, mask, nd);
		else
			return generic_permission(inode, submask, NULL);
	}

	spin_lock(&rinode->ri_lock);
	chain = chain_get(rinode->ri_chain);
	spin_unlock(&rinode->ri_lock);

	args.args.i_permission.inode = inode;
	args.args.i_permission.mask = mask;
	args.args.i_permission.nd = nd;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISREG(inode->i_mode))
		args.type.id = RFS_REG_IOP_PERMISSION;
	else if (S_ISDIR(inode->i_mode))
		args.type.id = RFS_DIR_IOP_PERMISSION;
	else if (S_ISLNK(inode->i_mode))
		args.type.id = RFS_LNK_IOP_PERMISSION;
	else if (S_ISCHR(inode->i_mode))
		args.type.id = RFS_CHR_IOP_PERMISSION;
	else if (S_ISBLK(inode->i_mode))
		args.type.id = RFS_BLK_IOP_PERMISSION;
	else if (S_ISFIFO(inode->i_mode))
		args.type.id = RFS_FIFO_IOP_PERMISSION;
	else 
		args.type.id = RFS_SOCK_IOP_PERMISSION;

	if (!rfs_precall_flts(0, chain, &cont, &args)) {
		if (rinode->ri_op_old && rinode->ri_op_old->permission)
			rv = rinode->ri_op_old->permission(args.args.i_permission.inode, args.args.i_permission.mask, args.args.i_permission.nd);
		else {
			submask = args.args.i_permission.mask & ~MAY_APPEND;
			rv = generic_permission(args.args.i_permission.inode, submask, NULL);
		}

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);
	rv = args.retv.rv_int;

	rinode_put(rinode);
	chain_put(chain);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_setattr(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	struct rinode *rinode = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;

	rinode = rinode_find(inode);
	if (!rinode) {
		if (inode && inode->i_op && inode->i_op->setattr)
			 return inode->i_op->setattr(dentry, iattr);

		return rv;
	}

	spin_lock(&rinode->ri_lock);
	chain = chain_get(rinode->ri_chain);
	spin_unlock(&rinode->ri_lock);

	args.args.i_setattr.dentry = dentry;
	args.args.i_setattr.iattr = iattr;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISREG(inode->i_mode))
		args.type.id = RFS_REG_IOP_SETATTR;
	else if (S_ISDIR(inode->i_mode))
		args.type.id = RFS_DIR_IOP_SETATTR;
	else if (S_ISLNK(inode->i_mode))
		args.type.id = RFS_LNK_IOP_SETATTR;
	else if (S_ISCHR(inode->i_mode))
		args.type.id = RFS_CHR_IOP_SETATTR;
	else if (S_ISBLK(inode->i_mode))
		args.type.id = RFS_BLK_IOP_SETATTR;
	else if (S_ISFIFO(inode->i_mode))
		args.type.id = RFS_FIFO_IOP_SETATTR;
	else 
		args.type.id = RFS_SOCK_IOP_SETATTR;

	if (!rfs_precall_flts(0, chain, &cont, &args)) {
		if (rinode->ri_op_old && rinode->ri_op_old->setattr)
			rv = rinode->ri_op_old->setattr(args.args.i_setattr.dentry, args.args.i_setattr.iattr);

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(0, chain, &cont, &args);
	rv = args.retv.rv_int;

	chain_put(chain);
	rinode_put(rinode);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

static int rfs_readpage_call(struct filter *flt, struct file *file, struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct rinode *rinode = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;
	int idx_start = 0;

	rinode = rinode_find(inode);
	if (!rinode) {
		if (inode && inode->i_mapping && inode->i_mapping->a_ops &&
				inode->i_mapping->a_ops->readpage)
			return inode->i_mapping->a_ops->readpage(file, page);

		return -EINVAL;
	}

	spin_lock(&rinode->ri_lock);
	chain = chain_get(rinode->ri_chain);
	spin_unlock(&rinode->ri_lock);

	args.args.a_readpage.file = file;
	args.args.a_readpage.page = page;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISREG(inode->i_mode))
		args.type.id = RFS_REG_AOP_READPAGE;
	else
		BUG();

	if (flt) {
		idx_start = chain_find_flt(chain, flt);

		if (idx_start == -1)
			idx_start = chain->c_flts_nr;
		else
			idx_start++;
	}

	if (!rfs_precall_flts(idx_start, chain, &cont, &args)) {
		if (rinode->ri_aop_old && rinode->ri_aop_old->readpage)
			rv = rinode->ri_aop_old->readpage(args.args.a_readpage.file,
						args.args.a_readpage.page);
		else
			rv = -EINVAL;

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(idx_start, chain, &cont, &args);
	rv = args.retv.rv_int;

	chain_put(chain);
	rinode_put(rinode);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_readpage_subcall(rfs_filter flt, union rfs_op_args *args)
{
	return rfs_readpage_call(flt, args->a_readpage.file, args->a_readpage.page);
}

int rfs_readpage(struct file *file, struct page *page)
{
	return rfs_readpage_call(NULL, file, page);
}


static int rfs_writepage_call(struct filter *flt, struct page *page, struct writeback_control *wbc)
{
	struct inode *inode = page->mapping->host;
	struct rinode *rinode = NULL;
	struct chain *chain = NULL;
	struct rfs_args args;
	struct context cont;
	int rv = 0;
	int idx_start = 0;

	rinode = rinode_find(inode);
	if (!rinode) {
		if (inode && inode->i_mapping && inode->i_mapping->a_ops &&
				inode->i_mapping->a_ops->writepage)
			return inode->i_mapping->a_ops->writepage(page, wbc);

		return -EINVAL;
	}

	spin_lock(&rinode->ri_lock);
	chain = chain_get(rinode->ri_chain);
	spin_unlock(&rinode->ri_lock);

	args.args.a_writepage.page = page;
	args.args.a_writepage.wbc = wbc;

	INIT_LIST_HEAD(&cont.data_list);

	if (S_ISREG(inode->i_mode))
		args.type.id = RFS_REG_AOP_WRITEPAGE;
	else
		BUG();

	if (flt) {
		idx_start = chain_find_flt(chain, flt);

		if (idx_start == -1)
			idx_start = chain->c_flts_nr;
		else
			idx_start++;
	}

	if (!rfs_precall_flts(idx_start, chain, &cont, &args)) {
		if (rinode->ri_aop_old && rinode->ri_aop_old->writepage)
			rv = rinode->ri_aop_old->writepage(args.args.a_writepage.page,
						args.args.a_writepage.wbc);
		else
			rv = -EINVAL;

		args.retv.rv_int = rv;
	}

	rfs_postcall_flts(idx_start, chain, &cont, &args);
	rv = args.retv.rv_int;

	chain_put(chain);
	rinode_put(rinode);

	BUG_ON(!list_empty(&cont.data_list));

	return rv;
}

int rfs_writepage_subcall(rfs_filter flt, union rfs_op_args *args)
{
	return rfs_writepage_call(flt, args->a_writepage.page, args->a_writepage.wbc);
}

int rfs_writepage(struct page *page, struct writeback_control *wbc)
{
	return rfs_writepage_call(NULL, page, wbc);
}

static void rinode_set_reg_ops(struct rinode *rinode, char *ops)
{
	if (ops[RFS_REG_IOP_PERMISSION])
		rinode->ri_op_new.permission = rfs_permission;
	else
		rinode->ri_op_new.permission = rinode->ri_op_old ? rinode->ri_op_old->permission : NULL;

	if (ops[RFS_REG_IOP_SETATTR])
		rinode->ri_op_new.setattr = rfs_setattr;
	else
		rinode->ri_op_new.setattr = rinode->ri_op_old ? rinode->ri_op_old->setattr : NULL;

	if (ops[RFS_REG_AOP_READPAGE])
		rinode->ri_aop_new.readpage = rfs_readpage;
	else
		rinode->ri_aop_new.readpage = rinode->ri_aop_old ? rinode->ri_aop_old->readpage : NULL;

	if (ops[RFS_REG_AOP_WRITEPAGE])
		rinode->ri_aop_new.writepage = rfs_writepage;
	else
		rinode->ri_aop_new.writepage = rinode->ri_aop_old ? rinode->ri_aop_old->writepage : NULL;
}

static void rinode_set_dir_ops(struct rinode *rinode, char *ops)
{
	if (ops[RFS_DIR_IOP_UNLINK])
		rinode->ri_op_new.unlink = rfs_unlink;
	else
		rinode->ri_op_new.unlink = rinode->ri_op_old ? rinode->ri_op_old->unlink : NULL;

	if (ops[RFS_DIR_IOP_PERMISSION])
		rinode->ri_op_new.permission = rfs_permission;
	else
		rinode->ri_op_new.permission = rinode->ri_op_old ? rinode->ri_op_old->permission : NULL;

	if (ops[RFS_DIR_IOP_SETATTR])
		rinode->ri_op_new.setattr = rfs_setattr;
	else
		rinode->ri_op_new.setattr = rinode->ri_op_old ? rinode->ri_op_old->setattr : NULL;

	if (ops[RFS_DIR_IOP_RMDIR])
		rinode->ri_op_new.rmdir = rfs_rmdir;
	else
		rinode->ri_op_new.rmdir = rinode->ri_op_old ? rinode->ri_op_old->rmdir : NULL;

	rinode->ri_op_new.mkdir = rfs_mkdir;
	rinode->ri_op_new.create = rfs_create;
	rinode->ri_op_new.link = rfs_link;
	rinode->ri_op_new.mknod = rfs_mknod;
	rinode->ri_op_new.symlink = rfs_symlink;
}

static void rinode_set_chr_ops(struct rinode *rinode, char *ops)
{
	if (ops[RFS_CHR_IOP_PERMISSION])
		rinode->ri_op_new.permission = rfs_permission;
	else
		rinode->ri_op_new.permission = rinode->ri_op_old ? rinode->ri_op_old->permission : NULL;

	if (ops[RFS_CHR_IOP_SETATTR])
		rinode->ri_op_new.setattr = rfs_setattr;
	else
		rinode->ri_op_new.setattr = rinode->ri_op_old ? rinode->ri_op_old->setattr : NULL;
}

static void rinode_set_blk_ops(struct rinode *rinode, char *ops)
{
	if (ops[RFS_BLK_IOP_PERMISSION])
		rinode->ri_op_new.permission = rfs_permission;
	else
		rinode->ri_op_new.permission = rinode->ri_op_old ? rinode->ri_op_old->permission : NULL;

	if (ops[RFS_BLK_IOP_SETATTR])
		rinode->ri_op_new.setattr = rfs_setattr;
	else
		rinode->ri_op_new.setattr = rinode->ri_op_old ? rinode->ri_op_old->setattr : NULL;
}

static void rinode_set_fifo_ops(struct rinode *rinode, char *ops)
{
	if (ops[RFS_FIFO_IOP_PERMISSION])
		rinode->ri_op_new.permission = rfs_permission;
	else
		rinode->ri_op_new.permission = rinode->ri_op_old ? rinode->ri_op_old->permission : NULL;

	if (ops[RFS_FIFO_IOP_SETATTR])
		rinode->ri_op_new.setattr = rfs_setattr;
	else
		rinode->ri_op_new.setattr = rinode->ri_op_old ? rinode->ri_op_old->setattr : NULL;
}

static void rinode_set_lnk_ops(struct rinode *rinode, char *ops)
{
	if (ops[RFS_LNK_IOP_PERMISSION])
		rinode->ri_op_new.permission = rfs_permission;
	else
		rinode->ri_op_new.permission = rinode->ri_op_old ? rinode->ri_op_old->permission : NULL;

	if (ops[RFS_LNK_IOP_SETATTR])
		rinode->ri_op_new.setattr = rfs_setattr;
	else
		rinode->ri_op_new.setattr = rinode->ri_op_old ? rinode->ri_op_old->setattr : NULL;
}

static void rinode_set_sock_ops(struct rinode *rinode, char *ops)
{
	if (ops[RFS_SOCK_IOP_PERMISSION])
		rinode->ri_op_new.permission = rfs_permission;
	else
		rinode->ri_op_new.permission = rinode->ri_op_old ? rinode->ri_op_old->permission : NULL;

	if (ops[RFS_SOCK_IOP_SETATTR])
		rinode->ri_op_new.setattr = rfs_setattr;
	else
		rinode->ri_op_new.setattr = rinode->ri_op_old ? rinode->ri_op_old->setattr : NULL;
}

void rinode_set_ops(struct rinode *rinode, struct ops *ops)
{
	umode_t mode = rinode->ri_inode->i_mode;

	if (S_ISREG(mode))
		rinode_set_reg_ops(rinode, ops->o_ops);

	else if (S_ISDIR(mode))
		rinode_set_dir_ops(rinode, ops->o_ops);

	else if (S_ISLNK(mode))
		rinode_set_lnk_ops(rinode, ops->o_ops);

	else if (S_ISCHR(mode))
		rinode_set_chr_ops(rinode, ops->o_ops);

	else if (S_ISBLK(mode))
		rinode_set_blk_ops(rinode, ops->o_ops);

	else if (S_ISFIFO(mode))
		rinode_set_fifo_ops(rinode, ops->o_ops);

	else if (S_ISSOCK(mode))
		rinode_set_sock_ops(rinode, ops->o_ops);

	rinode->ri_op_new.lookup = rfs_lookup;
}

int rinode_cache_create(void)
{
	rinode_cache = kmem_cache_create("rinode_cache",
					  sizeof(struct rinode),
					  0, SLAB_RECLAIM_ACCOUNT,
					  NULL, NULL);
	if (!rinode_cache)
		return -ENOMEM;

	return 0;
}

void rinode_cache_destroy(void)
{
	kmem_cache_destroy(rinode_cache);
}

int rfs_attach_data_inode(rfs_filter filter, struct inode *inode,
		struct rfs_priv_data *data, struct rfs_priv_data **exist)
{
	struct filter *flt;
	struct rinode *rinode;
	struct rfs_priv_data *found;

	flt = (struct filter *)filter;

	if (!flt || !inode || !data || !exist)
		return -EINVAL;

	rinode = rinode_find(inode);
	if (!rinode)
		return -ENODATA;

	spin_lock(&rinode->ri_lock);
	
	if (chain_find_flt(rinode->ri_chain, flt) == -1) {
		spin_unlock(&rinode->ri_lock);
		rinode_put(rinode);
		return -ENOENT;
	}

	found = rfs_find_data(&rinode->ri_data, flt);
	if (found) {
		*exist = rfs_get_data(found);
		spin_unlock(&rinode->ri_lock);
		rinode_put(rinode);
		return -EEXIST;
	}

	rfs_get_data(data);
	list_add_tail(&data->list, &rinode->ri_data);
	*exist = NULL;
	spin_unlock(&rinode->ri_lock);

	rinode_put(rinode);

	return 0;
}

int rfs_detach_data_inode(rfs_filter filter, struct inode *inode,
		struct rfs_priv_data **data)
{
	struct filter *flt;
	struct rinode *rinode;
	struct rfs_priv_data *found;

	flt = (struct filter *)filter;
	
	if (!flt || !inode || !data)
		return -EINVAL;

	rinode = rinode_find(inode);
	if (!rinode)
		return -ENODATA;

	spin_lock(&rinode->ri_lock);

	found = rfs_find_data(&rinode->ri_data, flt);
	if (!found) {
		spin_unlock(&rinode->ri_lock);
		rinode_put(rinode);
		return -ENODATA;
	}

	list_del(&found->list);
	*data = found;

	spin_unlock(&rinode->ri_lock);

	rinode_put(rinode);

	return 0;
}

int rfs_get_data_inode(rfs_filter filter, struct inode *inode,
		struct rfs_priv_data **data)
{
	struct filter *flt;
	struct rinode *rinode;
	struct rfs_priv_data *found;

	flt = (struct filter *)filter;
	
	if (!flt || !inode || !data)
		return -EINVAL;

	rinode = rinode_find(inode);
	if (!rinode)
		return -ENODATA;

	spin_lock(&rinode->ri_lock);
	found = rfs_find_data(&rinode->ri_data, flt);
	if (!found) {
		spin_unlock(&rinode->ri_lock);
		rinode_put(rinode);
		return -ENODATA;
	}

	*data = rfs_get_data(found);

	spin_unlock(&rinode->ri_lock);

	rinode_put(rinode);

	return 0;
}

EXPORT_SYMBOL(rfs_attach_data_inode);
EXPORT_SYMBOL(rfs_detach_data_inode);
EXPORT_SYMBOL(rfs_get_data_inode);
EXPORT_SYMBOL(rfs_readpage_subcall);
EXPORT_SYMBOL(rfs_writepage_subcall);

